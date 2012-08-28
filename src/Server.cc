/* Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "BindTransport.h"
#include "Server.h"
#include "ServiceManager.h"
#include "ShortMacros.h"

namespace RAMCloud {
/**
 * Constructor for Server: binds a configuration to a Server, but doesn't
 * start anything up yet.
 *
 * \param context
 *      Overall information about the RAMCloud server.  Note: if caller
 *      has not provided a serverList here, then we will.
 * \param config
 *      Specifies which services and their configuration details for
 *      when the Server is run.
 */
Server::Server(Context* context, const ServerConfig& config)
    : context(context)
    , config(config)
    , backupReadSpeed()
    , backupWriteSpeed()
    , serverId()
    , serverList(NULL)
    , failureDetector()
    , master()
    , backup()
    , membership()
    , ping()
{
    context->coordinatorSession->setLocation(
            config.coordinatorLocator.c_str());
}

/**
 * Destructor for Server.
 */
Server::~Server()
{
    delete serverList;
}

/**
 * Create services according to #config, enlist with the coordinator and
 * then return. This method should almost exclusively be used by MockCluster
 * and is only useful for unit testing.  Production code should always use
 * run() instead.
 *
 * \param bindTransport
 *      The BindTransport to register on to listen for rpcs during unit
 *      testing.
 */
void
Server::startForTesting(BindTransport& bindTransport)
{
    ServerId formerServerId = createAndRegisterServices(&bindTransport);
    enlist(formerServerId);
}

/**
 * Create services according to #config and enlist with the coordinator.
 * Either call this method or startForTesting(), not both.  Loops
 * forever calling Dispatch::poll() to serve requests.
 */
void
Server::run()
{
    ServerId formerServerId = createAndRegisterServices(NULL);

    // Only pin down memory _after_ users of LargeBlockOfMemory have
    // obtained their allocations (since LBOM probes are much slower if
    // the memory needs to be pinned during mmap).
    pinAllMemory();

    // The following statement suppresses a "long gap" message that would
    // otherwise be generated by the next call to dispatch.poll (the
    // warning is benign, and is caused by the time to benchmark secondary
    // storage above.
    Dispatch& dispatch = *context->dispatch;
    dispatch.currentTime = Cycles::rdtsc();

    enlist(formerServerId);

    while (true)
        dispatch.poll();
}

// - private -

/**
 * Create each of the services which are marked as active in config.services,
 * configure them according to #config, and register them with the
 * ServiceManager (or, if bindTransport is supplied, with the transport).
 *
 * \param bindTransport
 *      If given register the services with \a bindTransport instead of
 *      the Context's ServiceManager.
 * \return
 *      If this server is rejoining a cluster its former server id is returned,
 *      otherwise an invalid server is returned.  "Rejoining" means the backup
 *      service on this server may have segment replicas stored that were
 *      created by masters in the cluster.  In this case, the coordinator must
 *      be told of the former server id under which these replicas were
 *      created to ensure correct garbage collection of the stored replicas.
 */
ServerId
Server::createAndRegisterServices(BindTransport* bindTransport)
{
    ServerId formerServerId;

    if (config.services.has(WireFormat::COORDINATOR_SERVICE)) {
        DIE("Server class is not capable of running the CoordinatorService "
            "(yet).");
    }

    // If a serverList was already provided in the context, then use it;
    // otherwise, create a new one.
    if (context->serverList == NULL) {
        serverList = new ServerList(context);
    }

    if (config.services.has(WireFormat::MASTER_SERVICE)) {
        LOG(NOTICE, "Master is using %u backups", config.master.numReplicas);
        master.construct(context, config);
        if (bindTransport) {
            bindTransport->addService(*master,
                                      config.localLocator,
                                      WireFormat::MASTER_SERVICE);
        } else {
            context->serviceManager->addService(*master,
                                               WireFormat::MASTER_SERVICE);
        }
    }

    if (config.services.has(WireFormat::BACKUP_SERVICE)) {
        backup.construct(context, config);
        formerServerId = backup->getFormerServerId();
        if (config.backup.mockSpeed == 0) {
            backup->benchmark(backupReadSpeed, backupWriteSpeed);
        } else {
            backupReadSpeed = backupWriteSpeed = config.backup.mockSpeed;
        }
        if (bindTransport) {
            bindTransport->addService(*backup,
                                      config.localLocator,
                                      WireFormat::BACKUP_SERVICE);
        } else {
            context->serviceManager->addService(*backup,
                                               WireFormat::BACKUP_SERVICE);
        }
    }

    if (config.services.has(WireFormat::MEMBERSHIP_SERVICE)) {
        membership.construct(serverId,
            *static_cast<ServerList*>(context->serverList));
        if (bindTransport) {
            bindTransport->addService(*membership,
                                      config.localLocator,
                                      WireFormat::MEMBERSHIP_SERVICE);
        } else {
            context->serviceManager->addService(*membership,
                                               WireFormat::MEMBERSHIP_SERVICE);
        }
    }

    if (config.services.has(WireFormat::PING_SERVICE)) {
        ping.construct(context);
        if (bindTransport) {
            bindTransport->addService(*ping,
                                      config.localLocator,
                                      WireFormat::PING_SERVICE);
        } else {
            context->serviceManager->addService(*ping,
                                               WireFormat::PING_SERVICE);
        }
    }

    return formerServerId;
}

/**
 * Enlist the Server with the coordinator and start the failure detector
 * if it is enabled in #config.
 *
 * \param replacingId
 *      If this server has found replicas on storage written by a now-defunct
 *      server then the backup must report the server id that formerly owned
 *      those replicas upon enlistment. This is used to ensure that all
 *      servers in the cluster know of the crash of the old server which
 *      created the replicas before a new server enters the cluster
 *      attempting to reuse those replicas.  This property is used as part
 *      of the backup's replica garbage collection routines.
 */
void
Server::enlist(ServerId replacingId)
{
    // Enlist with the coordinator just before dedicating this thread
    // to rpc dispatch. This reduces the window of being unavailable to
    // service rpcs after enlisting with the coordinator (which can
    // lead to session open timeouts).
    serverId = CoordinatorClient::enlistServer(context, replacingId,
            config.services, config.localLocator, backupReadSpeed,
            backupWriteSpeed);

    if (master)
        master->init(serverId);
    if (backup)
        backup->init(serverId);
    if (config.detectFailures) {
        failureDetector.construct(
            context,
            serverId);
        failureDetector->start();
    }
}

} // namespace RAMCloud

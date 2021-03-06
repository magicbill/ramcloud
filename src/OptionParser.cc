/* Copyright (c) 2010-2013 Stanford University
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

#include <boost/version.hpp>
#include <boost/program_options.hpp>
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <fstream>

#include "Common.h"
#include "Transport.h"
#include "OptionParser.h"
#include "PcapFile.h"
#include "ShortMacros.h"

namespace RAMCloud {

/**
 * Parse the common RAMCloud command line options and store
 * them in #options.
 *
 * \param argc
 *      Count of the number of elements in argv.
 * \param argv
 *      A Unix style word tokenized array of command arguments including
 *      the executable name as argv[0].
 */
OptionParser::OptionParser(int argc,
                           char* argv[])
    : options()
    , allOptions("Usage")
    , appOptions()
{
    setup(argc, argv);
}

/**
 * Parse the common RAMCloud command line options and store
 * them in #options as well as additional application-specific options.
 *
 * \param appOptions
 *      The OptionsDescription to be parsed.  Each of the options that are
 *      part of it should include a pointer to where the value should be
 *      stored after parsing.  See boost::program_options::options_description
 *      for details.
 * \param argc
 *      Count of the number of elements in argv.
 * \param argv
 *      A Unix style word tokenized array of command arguments including
 *      the executable name as argv[0].
 */
OptionParser::OptionParser(
        const OptionsDescription& appOptions,
        int argc,
        char* argv[])
    : options()
    , allOptions("Usage")
    , appOptions(appOptions)
{
    setup(argc, argv);
}

/// Print program option documentation to stderr.
void
OptionParser::usage() const
{
    std::cerr << allOptions << std::endl;
}

/**
 * Print program option documentation to stderr and exit the program with
 * failure.
 */
void
OptionParser::usageAndExit() const
{
    usage();
    exit(EXIT_FAILURE);
}

/**
 * Internal method to do the heavy lifting of parsing the command line.
 *
 * Parses both application-specific and common options aborting and
 * printing usage if the --help flag is encountered.
 *
 * \param argc
 *      Count of the number of elements in argv.
 * \param argv
 *      A Unix style word tokenized array of command arguments including
 *      the executable name as argv[0].
 */
void
OptionParser::setup(int argc, char* argv[])
{
    namespace po = ProgramOptions;
    try {
        string defaultLogLevel;
        string logFile;
        vector<string> logLevels;
        string configFile(".ramcloud");

        // Basic options supported on the command line of all apps
        OptionsDescription commonOptions("Common");
        commonOptions.add_options()
            ("help", "Produce help message")
            ("c,config",
             po::value<string>(&configFile)->
                default_value(".ramcloud"),
             "Specify a path to a config file");

        // Options allowed on command line and in config file for all apps
        OptionsDescription configOptions("RAMCloud");
        configOptions.add_options()
            ("logFile",
             po::value<string>(&logFile),
             "File to use for log messages")
            ("logLevel,l",
             po::value<string>(&defaultLogLevel)->
                default_value("NOTICE"),
             "Default log level for all modules, see LogLevel")
            ("logModule",
             po::value<vector<string> >(&logLevels),
             "One or more module-specific log levels, specified in the form "
             "moduleName=level")
            ("coordinator,C",
             po::value<string>(&options.coordinatorLocator)->
               default_value("fast+udp:host=0.0.0.0,port=12246"),
             "Service locator where the coordinator can be contacted")
            ("externalStorage,x",
             po::value<string>(&options.externalStorageLocator),
             "Locator for external storage server containing cluster "
             "configuration information")
            ("local,L",
             po::value<string>(&options.localLocator)->
               default_value("fast+udp:host=0.0.0.0,port=12242"),
             "Service locator to listen on")
            ("pcapFile",
             po::value<string>(&options.pcapFilePath)->
               default_value(""),
             "File to log transmitted and received packets to in pcap format. "
             "Only works with InfUdDriver based transports for now; "
             "use tcpdump to capture kernel-based packet formats.")
            ("timeout",
             ProgramOptions::value<uint32_t>(&options.sessionTimeout)->
                default_value(0),
             "How long transports should wait (ms) before declaring that a "
             "client connection for each rpc session is dead."
             "0 means use transport-specific default.")
            ("portTimeout",
             ProgramOptions::value<int32_t>(&options.portTimeout)->
                default_value(-1), // Overriding to the initial value.
             "How long transports should wait (ms) before declaring that a "
             "server connection for listening client requests is dead."
             "0 means use transport-specific default."
             "Negative number means disabling the timer.");

        // Do one pass with just help/config file options so we can get
        // the alternate config file location, if specified.  Then
        // do a second pass with all the options for real.
        po::variables_map throwAway;
        // Need to skip unknown parameters since we'll repeat the parsing
        // once we know which config file to read in.
        po::store(po::command_line_parser(argc, argv).options(commonOptions)
                                                     .allow_unregistered()
                                                     .run(),
                  throwAway);
        po::notify(throwAway);

        allOptions.add(commonOptions).add(configOptions).add(appOptions);

        po::variables_map vm;
        std::ifstream configInput(configFile.c_str());
        po::store(po::parse_command_line(argc, argv, allOptions), vm);
        // true here lets config files contain unknown key/value pairs
        // this lets a config file be used for multiple programs
        po::store(po::parse_config_file(configInput, allOptions, true), vm);
        po::notify(vm);

        if (vm.count("help"))
            usageAndExit();

        if (logFile.size() != 0)
            Logger::get().setLogFile(logFile.c_str());
        Logger::get().setLogLevels(defaultLogLevel);
        foreach (auto moduleLevel, logLevels) {
            auto pos = moduleLevel.find("=");
            if (pos == string::npos) {
                LOG(WARNING, "Bad log module level format: %s, "
                    "example moduleName=3", moduleLevel.c_str());
                continue;
            }
            auto name = moduleLevel.substr(0, pos);
            auto level = moduleLevel.substr(pos + 1);
            Logger::get().setLogLevel(name, level);
        }

        if (options.pcapFilePath != "")
            pcapFile.construct(options.pcapFilePath.c_str(),
                               PcapFile::LinkType::ETHERNET);
    }
    catch (po::multiple_occurrences& e) {
        // This clause provides a more understandable error message
        // (the default is fairly opaque).
#if BOOST_VERSION >= 104200 // get_option_name introduced in Boost 1.4.2
        throw po::error(format("command-line option '%s' occurs multiple times",
                e.get_option_name().c_str()));
#else
        throw po::error("command-line option occurs multiple times");
#endif
    }
}

} // end RAMCloud

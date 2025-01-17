#include <iostream>
#include <boost/program_options.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/filesystem.hpp>

#include "shinysocks.h"
#include "logging.h"

using namespace std;
using namespace shinysocks;
using boost::asio::ip::tcp;

void SleepUntilDoomdsay()
{
    boost::asio::io_service main_thread_service;

    boost::asio::signal_set signals(main_thread_service, SIGINT, SIGTERM
#ifdef SIGQUIT
        ,SIGQUIT
#endif
        );
    signals.async_wait([](boost::system::error_code /*ec*/, int signo) {

        LOG_INFO << "Reiceived signal " << signo << ". Shutting down";
    });

    LOG_DEBUG << "Main thread going to sleep - waiting for shtudown signal";
    main_thread_service.run();
    LOG_DEBUG << "Main thread is awake";
}

int main(int argc, char **argv) {

    namespace po = boost::program_options;
    namespace pt = boost::property_tree;
    bool run_as_daemon = false;

    pt::ptree opts;
    vector<unique_ptr<Listener>> listeners;

    {
        std::string log_level = "info";
        std::string conf_file = "shinysocks.conf";

        po::options_description general("General Options");

        general.add_options()
            ("help,h", "Print help and exit")
            ("version", "Print version and exit")
            ("config-file,c",
                po::value<string>(&conf_file)->default_value(conf_file),
                "Configuration file")
#ifndef WIN32
            ("daemon",  po::value<bool>(&run_as_daemon), "Run as a system daemon")
#endif
            ("log-level,l",
                 po::value<string>(&log_level)->default_value(log_level),
                 "Log-level to use; one of 'info', 'debug', 'trace'")
            ;

        po::options_description cmdline_options;
        cmdline_options.add(general);

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, cmdline_options), vm);
        po::notify(vm);

        if (vm.count("help")) {
            cout << GetProgramName() << ' ' << GetProgramVersion()
				<< cmdline_options << endl;
            return -1;
        }

        if (vm.count("version")) {
            cout << GetProgramName() << ' ' << GetProgramVersion() << endl;
            return -1;
        }

        auto llevel = logfault::LogLevel::INFO;
        if (log_level == "debug") {
            llevel = logfault::LogLevel::DEBUGGING;
        } else if (log_level == "trace") {
            llevel = logfault::LogLevel::TRACE;
        } else if (log_level == "info") {
            ;  // Do nothing
        } else {
            std::cerr << "Unknown log-level: " << log_level << endl;
            return -1;
        }

        if (!boost::filesystem::exists(conf_file)) {
            cerr << "*** The configuration-file '"
                << conf_file
                << "' does not exist.";
            return -1;
        }
        read_info(conf_file, opts);

        if (auto log_file = opts.get_optional<string>("log.file")) {
            cout << "Opening log-file: " << *log_file << endl;
            logfault::LogManager::Instance().AddHandler(
                        make_unique<logfault::StreamHandler>(*log_file, llevel));
        } else {
            logfault::LogManager::Instance().AddHandler(
                        make_unique<logfault::StreamHandler>(clog, llevel));
        }

        LOG_INFO << GetProgramName() << ' ' << GetProgramVersion()
                                << " starting up. Log level: " << log_level;
    }

    Manager::Conf conf;
    if (auto threads = opts.get_optional<int>("system.io-threads")) {
        conf.io_threads = *threads;
    }


    // Start acceptor(s)
    Manager manager(conf);
    {
        boost::asio::io_service io_service;
        for(auto &node :  opts.get_child("interfaces")) {

            auto host = node.second.get<string>("hostname");
            auto port = node.second.get<string>("port");

            LOG_INFO << "Resolving host=" << host << ", port=" << port;

            tcp::resolver resolver(io_service);
            auto address_it = resolver.resolve({host, port});
            decltype(address_it) addr_end;

            for(; address_it != addr_end; ++address_it) {
                auto iface = make_unique<Listener>(manager, *address_it);
                iface->StartAccepting();
                listeners.push_back(move(iface));
            }
        }
    }

#ifndef WIN32
    if (run_as_daemon) {
        LOG_INFO << "Switching to system daemon mode";
        daemon(1, 0);
    }
#endif


    // Wait for signal
    SleepUntilDoomdsay();

    manager.Shutdown();

    // Just wait for the IO thread(s) to end.
    manager.WaitForAllThreads();

    return 0;
}

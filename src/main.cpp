#include "MCTPBinding.hpp"
#include "PCIeBinding.hpp"
#include "SMBusBinding.hpp"
#include "hw/nuvoton/PCIeDriver.hpp"
#include "hw/nuvoton/PCIeMonitor.hpp"

#include <CLI/CLI.hpp>
#include <boost/asio/signal_set.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/object_server.hpp>

std::shared_ptr<MctpBinding>
    getBindingPtr(const Configuration& configuration,
                  std::shared_ptr<sdbusplus::asio::connection> conn,
                  std::shared_ptr<object_server>& objectServer,
                  boost::asio::io_context& ioc)
{
    std::string mctpBaseObj = "/xyz/openbmc_project/mctp";

    if (auto smbusConfig =
            dynamic_cast<const SMBusConfiguration*>(&configuration))
    {
        return std::make_shared<SMBusBinding>(
            conn, objectServer, mctpBaseObj, *smbusConfig, ioc,
            std::make_unique<boost::asio::posix::stream_descriptor>(ioc));
    }
    else if (auto pcieConfig =
                 dynamic_cast<const PcieConfiguration*>(&configuration))
    {
        return std::make_shared<PCIeBinding>(
            conn, objectServer, mctpBaseObj, *pcieConfig, ioc,
            std::make_unique<hw::nuvoton::PCIeDriver>(ioc),
            std::make_unique<hw::nuvoton::PCIeMonitor>(ioc));
    }

    return nullptr;
}

int main(int argc, char* argv[])
{
    CLI::App app("MCTP Daemon");
    std::string binding;
    std::string configPath = "/usr/share/mctp/mctp_config.json";
    std::optional<std::pair<std::string, std::unique_ptr<Configuration>>>
        mctpdConfigurationPair;

    app.add_option("-b,--binding", binding,
                   "MCTP Physical Binding. Supported: -b smbus, -b pcie")
        ->required();
    app.add_option("-c,--config", configPath, "Path to configuration file.")
        ->capture_default_str();
    CLI11_PARSE(app, argc, argv);

    boost::asio::io_context ioc;
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    std::shared_ptr<MctpBinding> bindingPtr;
    signals.async_wait(
        [&ioc, &bindingPtr](const boost::system::error_code&, const int&) {
            // Ensure we destroy binding object before we do an ioc stop
            bindingPtr.reset();
            ioc.stop();
        });

    auto conn = std::make_shared<sdbusplus::asio::connection>(ioc);

    /* Process configuration */
    try
    {
        mctpdConfigurationPair = getConfiguration(conn, binding, configPath);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            (std::string("Exception: ") + e.what()).c_str());
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid configuration; exiting");
        return -1;
    }

    if (!mctpdConfigurationPair)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Could not load any configuration; exiting");
        return -1;
    }

    auto& [mctpdName, mctpdConfiguration] = *mctpdConfigurationPair;
    auto objectServer = std::make_shared<object_server>(conn, true);
    const std::string mctpServiceName = "xyz.openbmc_project." + mctpdName;
    conn->request_name(mctpServiceName.c_str());

    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Starting MCTP service: " + mctpServiceName).c_str());

    bindingPtr = getBindingPtr(*mctpdConfiguration, conn, objectServer, ioc);

    if (!bindingPtr)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Unable to create MCTP binding");
        return -1;
    }

    try
    {
        bindingPtr->setDbusName(mctpServiceName);
        bindingPtr->initializeBinding();
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            (std::string("Exception: ") + e.what()).c_str());
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to intialize MCTP binding; exiting");
        return -1;
    }
    ioc.run();

    return 0;
}

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/JsonObject.h>
#include <aws/crt/UUID.h>
#include <aws/crt/io/HostResolver.h>

#include <aws/iot/MqttClient.h>

#include <aws/iotidentity/CreateCertificateFromCsrRequest.h>
#include <aws/iotidentity/CreateCertificateFromCsrResponse.h>
#include <aws/iotidentity/CreateCertificateFromCsrSubscriptionRequest.h>
#include <aws/iotidentity/CreateKeysAndCertificateRequest.h>
#include <aws/iotidentity/CreateKeysAndCertificateResponse.h>
#include <aws/iotidentity/CreateKeysAndCertificateSubscriptionRequest.h>
#include <aws/iotidentity/ErrorResponse.h>
#include <aws/iotidentity/IotIdentityClient.h>
#include <aws/iotidentity/RegisterThingRequest.h>
#include <aws/iotidentity/RegisterThingResponse.h>
#include <aws/iotidentity/RegisterThingSubscriptionRequest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

#include "../../utils/CommandLineUtils.h"

using namespace Aws::Crt;
using namespace Aws::Iotidentity;
using namespace std::this_thread; // sleep_for, sleep_until
using namespace std::chrono;      // nanoseconds, system_clock, seconds

static void sleep(int sleeptime)
{
    std::cout << "Sleeping for " << sleeptime << " seconds..." << std::endl;
    sleep_until(system_clock::now() + seconds(sleeptime));
}

static std::string getFileData(std::string const &fileName)
{
    std::ifstream ifs(fileName);
    std::string str;
    getline(ifs, str, (char)ifs.eof());
    return str;
}

int main(int argc, char *argv[])
{
    /************************ Setup the Lib ****************************/
    /*
     * Do the global initialization for the API.
     */
    ApiHandle apiHandle;
    String clientId(String("test-") + Aws::Crt::UUID().ToString());
    String csrFile;

    String token;
    RegisterThingResponse registerThingResponse;

    /*********************** Parse Arguments ***************************/
    Utils::CommandLineUtils cmdUtils = Utils::CommandLineUtils();
    cmdUtils.RegisterProgramName("fleet-provisioning");
    cmdUtils.AddCommonMQTTCommands();
    cmdUtils.RegisterCommand("key", "<path>", "Path to your key in PEM format.");
    cmdUtils.RegisterCommand("cert", "<path>", "Path to your client certificate in PEM format.");
    cmdUtils.RegisterCommand("template_name", "<str>", "The name of your provisioning template");
    cmdUtils.RegisterCommand("template_parameters", "<json>", "Template parameters json");
    cmdUtils.RegisterCommand("csr", "<path>", "Path to CSR in PEM format (optional)");
    cmdUtils.AddLoggingCommands();
    const char **const_argv = (const char **)argv;
    cmdUtils.SendArguments(const_argv, const_argv + argc);
    cmdUtils.StartLoggingBasedOnCommand(&apiHandle);

    String templateName = cmdUtils.GetCommandRequired("template_name");
    String templateParameters = cmdUtils.GetCommandRequired("template_parameters");
    if (cmdUtils.HasCommand("csr"))
    {
        csrFile = getFileData(cmdUtils.GetCommand("csr").c_str()).c_str();
    }

    /* Get a MQTT client connection from the command parser */
    auto connection = cmdUtils.BuildMQTTConnection();

    /*
     * In a real world application you probably don't want to enforce synchronous behavior
     * but this is a sample console application, so we'll just do that with a condition variable.
     */
    std::promise<bool> connectionCompletedPromise;
    std::promise<void> connectionClosedPromise;

    /*
     * This will execute when an mqtt connect has completed or failed.
     */
    auto onConnectionCompleted = [&](Mqtt::MqttConnection &, int errorCode, Mqtt::ReturnCode returnCode, bool) {
        if (errorCode)
        {
            fprintf(stdout, "Connection failed with error %s\n", ErrorDebugString(errorCode));
            connectionCompletedPromise.set_value(false);
        }
        else
        {
            fprintf(stdout, "Connection completed with return code %d\n", returnCode);
            connectionCompletedPromise.set_value(true);
        }
    };

    /*
     * Invoked when a disconnect message has completed.
     */
    auto onDisconnect = [&](Mqtt::MqttConnection & /*conn*/) {
        {
            fprintf(stdout, "Disconnect completed\n");
            connectionClosedPromise.set_value();
        }
    };

    connection->OnConnectionCompleted = std::move(onConnectionCompleted);
    connection->OnDisconnect = std::move(onDisconnect);

    /*
     * Actually perform the connect dance.
     */
    fprintf(stdout, "Connecting...\n");
    if (!connection->Connect(clientId.c_str(), true, 0))
    {
        fprintf(stderr, "MQTT Connection failed with error %s\n", ErrorDebugString(connection->LastError()));
        exit(-1);
    }

    if (connectionCompletedPromise.get_future().get())
    {
        IotIdentityClient identityClient(connection);

        std::promise<void> csrPublishCompletedPromise;
        std::promise<void> csrAcceptedCompletedPromise;
        std::promise<void> csrRejectedCompletedPromise;

        std::promise<void> keysPublishCompletedPromise;
        std::promise<void> keysAcceptedCompletedPromise;
        std::promise<void> keysRejectedCompletedPromise;

        std::promise<void> registerPublishCompletedPromise;
        std::promise<void> registerAcceptedCompletedPromise;
        std::promise<void> registerRejectedCompletedPromise;

        auto onCsrPublishSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(stderr, "Error publishing to CreateCertificateFromCsr: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }

            csrPublishCompletedPromise.set_value();
        };

        auto onCsrAcceptedSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(
                    stderr, "Error subscribing to CreateCertificateFromCsr accepted: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }

            csrAcceptedCompletedPromise.set_value();
        };

        auto onCsrRejectedSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(
                    stderr, "Error subscribing to CreateCertificateFromCsr rejected: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }
            csrRejectedCompletedPromise.set_value();
        };

        auto onCsrAccepted = [&](CreateCertificateFromCsrResponse *response, int ioErr) {
            if (ioErr == AWS_OP_SUCCESS)
            {
                fprintf(
                    stdout, "CreateCertificateFromCsrResponse certificateId: %s.\n", response->CertificateId->c_str());
                token = *response->CertificateOwnershipToken;
            }
            else
            {
                fprintf(stderr, "Error on subscription: %s.\n", ErrorDebugString(ioErr));
                exit(-1);
            }
        };

        auto onCsrRejected = [&](ErrorResponse *error, int ioErr) {
            if (ioErr == AWS_OP_SUCCESS)
            {
                fprintf(
                    stdout,
                    "CreateCertificateFromCsr failed with statusCode %d, errorMessage %s and errorCode %s.",
                    *error->StatusCode,
                    error->ErrorMessage->c_str(),
                    error->ErrorCode->c_str());
                exit(-1);
            }
            else
            {
                fprintf(stderr, "Error on subscription: %s.\n", ErrorDebugString(ioErr));
                exit(-1);
            }
        };

        auto onKeysPublishSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(stderr, "Error publishing to CreateKeysAndCertificate: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }

            keysPublishCompletedPromise.set_value();
        };

        auto onKeysAcceptedSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(
                    stderr, "Error subscribing to CreateKeysAndCertificate accepted: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }

            keysAcceptedCompletedPromise.set_value();
        };

        auto onKeysRejectedSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(
                    stderr, "Error subscribing to CreateKeysAndCertificate rejected: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }
            keysRejectedCompletedPromise.set_value();
        };

        auto onKeysAccepted = [&](CreateKeysAndCertificateResponse *response, int ioErr) {
            if (ioErr == AWS_OP_SUCCESS)
            {
                fprintf(
                    stdout, "CreateKeysAndCertificateResponse certificateId: %s.\n", response->CertificateId->c_str());
                token = *response->CertificateOwnershipToken;
            }
            else
            {
                fprintf(stderr, "Error on subscription: %s.\n", ErrorDebugString(ioErr));
                exit(-1);
            }
        };

        auto onKeysRejected = [&](ErrorResponse *error, int ioErr) {
            if (ioErr == AWS_OP_SUCCESS)
            {
                fprintf(
                    stdout,
                    "CreateKeysAndCertificate failed with statusCode %d, errorMessage %s and errorCode %s.",
                    *error->StatusCode,
                    error->ErrorMessage->c_str(),
                    error->ErrorCode->c_str());
                exit(-1);
            }
            else
            {
                fprintf(stderr, "Error on subscription: %s.\n", ErrorDebugString(ioErr));
                exit(-1);
            }
        };

        auto onRegisterAcceptedSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(stderr, "Error subscribing to RegisterThing accepted: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }

            registerAcceptedCompletedPromise.set_value();
        };

        auto onRegisterRejectedSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(stderr, "Error subscribing to RegisterThing rejected: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }
            registerRejectedCompletedPromise.set_value();
        };

        auto onRegisterAccepted = [&](RegisterThingResponse *response, int ioErr) {
            if (ioErr == AWS_OP_SUCCESS)
            {
                fprintf(stdout, "RegisterThingResponse ThingName: %s.\n", response->ThingName->c_str());
            }
            else
            {
                fprintf(stderr, "Error on subscription: %s.\n", ErrorDebugString(ioErr));
                exit(-1);
            }
        };

        auto onRegisterRejected = [&](ErrorResponse *error, int ioErr) {
            if (ioErr == AWS_OP_SUCCESS)
            {
                fprintf(
                    stdout,
                    "RegisterThing failed with statusCode %d, errorMessage %s and errorCode %s.",
                    *error->StatusCode,
                    error->ErrorMessage->c_str(),
                    error->ErrorCode->c_str());
            }
            else
            {
                fprintf(stderr, "Error on subscription: %s.\n", ErrorDebugString(ioErr));
                exit(-1);
            }
        };

        auto onRegisterPublishSubAck = [&](int ioErr) {
            if (ioErr != AWS_OP_SUCCESS)
            {
                fprintf(stderr, "Error publishing to RegisterThing: %s\n", ErrorDebugString(ioErr));
                exit(-1);
            }

            registerPublishCompletedPromise.set_value();
        };

        if (csrFile.empty())
        {
            // CreateKeysAndCertificate workflow
            std::cout << "Subscribing to CreateKeysAndCertificate Accepted and Rejected topics" << std::endl;
            CreateKeysAndCertificateSubscriptionRequest keySubscriptionRequest;
            identityClient.SubscribeToCreateKeysAndCertificateAccepted(
                keySubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onKeysAccepted, onKeysAcceptedSubAck);

            identityClient.SubscribeToCreateKeysAndCertificateRejected(
                keySubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onKeysRejected, onKeysRejectedSubAck);

            std::cout << "Publishing to CreateKeysAndCertificate topic" << std::endl;
            CreateKeysAndCertificateRequest createKeysAndCertificateRequest;
            identityClient.PublishCreateKeysAndCertificate(
                createKeysAndCertificateRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onKeysPublishSubAck);

            std::cout << "Subscribing to RegisterThing Accepted and Rejected topics" << std::endl;
            RegisterThingSubscriptionRequest registerSubscriptionRequest;
            registerSubscriptionRequest.TemplateName = templateName;

            identityClient.SubscribeToRegisterThingAccepted(
                registerSubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onRegisterAccepted, onRegisterAcceptedSubAck);

            identityClient.SubscribeToRegisterThingRejected(
                registerSubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onRegisterRejected, onRegisterRejectedSubAck);

            sleep(1);

            std::cout << "Publishing to RegisterThing topic" << std::endl;
            RegisterThingRequest registerThingRequest;
            registerThingRequest.TemplateName = templateName;

            const Aws::Crt::String jsonValue = templateParameters;
            Aws::Crt::JsonObject value(jsonValue);
            Map<String, JsonView> pm = value.View().GetAllObjects();
            Aws::Crt::Map<Aws::Crt::String, Aws::Crt::String> params =
                Aws::Crt::Map<Aws::Crt::String, Aws::Crt::String>();

            for (const auto &x : pm)
            {
                params.emplace(x.first, x.second.AsString());
            }

            registerThingRequest.Parameters = params;
            registerThingRequest.CertificateOwnershipToken = token;

            identityClient.PublishRegisterThing(
                registerThingRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onRegisterPublishSubAck);
            sleep(1);

            keysPublishCompletedPromise.get_future().wait();
            keysAcceptedCompletedPromise.get_future().wait();
            keysRejectedCompletedPromise.get_future().wait();
            registerPublishCompletedPromise.get_future().wait();
            registerAcceptedCompletedPromise.get_future().wait();
            registerRejectedCompletedPromise.get_future().wait();
        }
        else
        {
            // CreateCertificateFromCsr workflow
            std::cout << "Subscribing to CreateCertificateFromCsr Accepted and Rejected topics" << std::endl;
            CreateCertificateFromCsrSubscriptionRequest csrSubscriptionRequest;
            identityClient.SubscribeToCreateCertificateFromCsrAccepted(
                csrSubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onCsrAccepted, onCsrAcceptedSubAck);

            identityClient.SubscribeToCreateCertificateFromCsrRejected(
                csrSubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onCsrRejected, onCsrRejectedSubAck);

            std::cout << "Publishing to CreateCertificateFromCsr topic" << std::endl;
            CreateCertificateFromCsrRequest createCertificateFromCsrRequest;
            createCertificateFromCsrRequest.CertificateSigningRequest = csrFile;
            identityClient.PublishCreateCertificateFromCsr(
                createCertificateFromCsrRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onCsrPublishSubAck);

            std::cout << "Subscribing to RegisterThing Accepted and Rejected topics" << std::endl;
            RegisterThingSubscriptionRequest registerSubscriptionRequest;
            registerSubscriptionRequest.TemplateName = templateName;

            identityClient.SubscribeToRegisterThingAccepted(
                registerSubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onRegisterAccepted, onRegisterAcceptedSubAck);

            identityClient.SubscribeToRegisterThingRejected(
                registerSubscriptionRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onRegisterRejected, onRegisterRejectedSubAck);

            sleep(2);

            std::cout << "Publishing to RegisterThing topic" << std::endl;
            RegisterThingRequest registerThingRequest;
            registerThingRequest.TemplateName = templateName;

            const Aws::Crt::String jsonValue = templateParameters;
            Aws::Crt::JsonObject value(jsonValue);
            Map<String, JsonView> pm = value.View().GetAllObjects();
            Aws::Crt::Map<Aws::Crt::String, Aws::Crt::String> params =
                Aws::Crt::Map<Aws::Crt::String, Aws::Crt::String>();

            for (const auto &x : pm)
            {
                params.emplace(x.first, x.second.AsString());
            }

            registerThingRequest.Parameters = params;
            registerThingRequest.CertificateOwnershipToken = token;

            identityClient.PublishRegisterThing(
                registerThingRequest, AWS_MQTT_QOS_AT_LEAST_ONCE, onRegisterPublishSubAck);
            sleep(2);

            csrPublishCompletedPromise.get_future().wait();
            csrAcceptedCompletedPromise.get_future().wait();
            csrRejectedCompletedPromise.get_future().wait();
            registerPublishCompletedPromise.get_future().wait();
            registerAcceptedCompletedPromise.get_future().wait();
            registerRejectedCompletedPromise.get_future().wait();
        }
    }

    /* Disconnect */
    if (connection->Disconnect())
    {
        connectionClosedPromise.get_future().wait();
    }

    return 0;
}

/*
	\file   mqtt_iothub_packetParameters.c

	\brief  MQTT Packet Parameters source file.

	(c) 2018 Microchip Technology Inc. and its subsidiaries.

	Subject to your compliance with these terms, you may use Microchip software and any
	derivatives exclusively with Microchip products. It is your responsibility to comply with third party
	license terms applicable to your use of third party software (including open source software) that
	may accompany Microchip software.

	THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
	EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY
	IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS
	FOR A PARTICULAR PURPOSE.

	IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
	INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
	WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP
	HAS BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO
	THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL
	CLAIMS IN ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT
	OF FEES, IF ANY, THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS
	SOFTWARE.
*/

// ToDo This file needs to be renamed as app_mqttClient.c
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include "mqtt/mqtt_core/mqtt_core.h"
#include "mqtt/mqtt_packetTransfer_interface.h"
#include "mqtt_packetPopulate.h"
#include "mqtt_iothub_packetPopulate.h"
#include "iot_config/IoT_Sensor_Node_config.h"
#include "../../debug_print.h"
#include "cryptoauthlib/lib/basic/atca_basic.h"
#include "azure/iot/az_iot_hub_client.h"
#include "azure/core/az_span.h"

pf_MQTT_CLIENT pf_mqqt_iothub_client = {
  MQTT_CLIENT_iothub_publish,
  MQTT_CLIENT_iothub_receive,
  MQTT_CLIENT_iothub_connect,
  MQTT_CLIENT_iothub_subscribe,
  MQTT_CLIENT_iothub_connected,  
};

extern void receivedFromCloud_c2d(uint8_t* topic, uint8_t* payload);
extern void receivedFromCloud_message(uint8_t* topic, uint8_t* payload);
extern void receivedFromCloud_twin(uint8_t* topic, uint8_t* payload);
extern void receivedFromCloud_patch(uint8_t* topic, uint8_t* payload);
static const az_span twin_request_id = AZ_SPAN_LITERAL_FROM_STR("initial_get world");

char mqtt_telemetry_topic_buf[64];
char mqtt_get_topic_twin_buf[64];
char username_buf[200];
char sas_token_buf[512];
uint8_t device_id_buf[100];
az_span device_id;
az_iot_hub_client hub_client;

/** \brief MQTT publish handler call back table.
 *
 * This callback table lists the callback function for to be called on reception
 * of a PUBLISH message for each topic which the application has subscribed to.
 * For each new topic which is subscribed to by the application, there needs to
 * be a corresponding publish handler.
 * E.g.: For a particular topic
 *       mchp/mySubscribedTopic/myDetailedPath
 *       Sample publish handler function  = void handlePublishMessage(uint8_t *topic, uint8_t *payload)
 */
publishReceptionHandler_t imqtt_publishReceiveCallBackTable[MAX_NUM_TOPICS_SUBSCRIBE];

void MQTT_CLIENT_iothub_publish(uint8_t* data, uint16_t len)
{
	uint8_t properties_buf[256];
	az_span properties = AZ_SPAN_FROM_BUFFER(properties_buf);
	az_iot_hub_client_properties properties_topic;
	az_result result = az_iot_hub_client_properties_init(&properties_topic, properties, 0);
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_properties_init failed");
		return;
	}

	result = az_iot_hub_client_properties_append(&properties_topic, AZ_SPAN_FROM_STR("foo"), AZ_SPAN_FROM_STR("bar"));
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_properties_append failed");
		return;
	}

	result = az_iot_hub_client_telemetry_get_publish_topic(
		&hub_client, &properties_topic, mqtt_telemetry_topic_buf, sizeof(mqtt_telemetry_topic_buf), NULL);

	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_telemetry_get_publish_topic failed");
		return;
	}

	mqttPublishPacket cloudPublishPacket;
	// Fixed header
	cloudPublishPacket.publishHeaderFlags.duplicate = AZ_HUB_CLIENT_DEFAULT_MQTT_TELEMETRY_DUPLICATE;
	cloudPublishPacket.publishHeaderFlags.qos = AZ_HUB_CLIENT_DEFAULT_MQTT_TELEMETRY_QOS;
	cloudPublishPacket.publishHeaderFlags.retain = AZ_HUB_CLIENT_DEFAULT_MQTT_TELEMETRY_RETAIN;
	// Variable header
	cloudPublishPacket.topic = (uint8_t*)mqtt_telemetry_topic_buf;

	// Payload
	cloudPublishPacket.payload = data;
	cloudPublishPacket.payloadLength = len;

	if (MQTT_CreatePublishPacket(&cloudPublishPacket) != true)
	{
		debug_printError("MQTT: Connection lost PUBLISH failed");
	}
}

void MQTT_CLIENT_iothub_receive(uint8_t* data, uint16_t len)
{
	MQTT_GetReceivedData(data, len);
}

void MQTT_CLIENT_iothub_connect(char* deviceID)
{
	const az_span iothub_hostname = AZ_SPAN_LITERAL_FROM_STR(CFG_MQTT_HUB_HOST);
	const az_span deviceID_parm = az_span_create_from_str(deviceID);
	az_span device_id = AZ_SPAN_FROM_BUFFER(device_id_buf);
	az_span_copy(device_id, deviceID_parm);
	device_id = az_span_slice(device_id, 0, az_span_size(deviceID_parm));

	az_iot_hub_client_options options = az_iot_hub_client_options_default();
	az_result result = az_iot_hub_client_init(&hub_client, iothub_hostname, device_id, &options);
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_init failed");
		return;
	}

	size_t username_buf_len;
	result = az_iot_hub_client_get_user_name(&hub_client, username_buf, sizeof(username_buf), &username_buf_len);
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_get_user_name failed");
		return;
	}

	time_t expire_time = time(NULL) + 60 * 60; // token expires in 1 hour
	uint8_t signature_buf[256];
	az_span signature = AZ_SPAN_FROM_BUFFER(signature_buf);
	result = az_iot_hub_client_sas_get_signature(&hub_client, expire_time, signature, &signature); 
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_sas_get_signature failed");
		return;
	}

	uint8_t key[32];
	size_t key_size = _az_COUNTOF(key);
	atcab_base64decode_(hub_device_key, strlen(hub_device_key), key, &key_size, az_iot_b64rules);
	atcab_nonce(key);
	uint8_t hash[32];
	ATCA_STATUS atca_status = atcab_sha_hmac(signature_buf, az_span_size(signature), ATCA_TEMPKEY_KEYID, hash, SHA_MODE_TARGET_OUT_ONLY);
	if (atca_status != ATCA_SUCCESS)
	{
		debug_printError("atcab_sha_hmac failed");
		return;
	}

	char signature_hash_buf[64];
	key_size = _az_COUNTOF(signature_hash_buf);
	atcab_base64encode_(hash, _az_COUNTOF(hash), signature_hash_buf, &key_size, az_iot_b64rules);

	char signature_hash_encoded_buf[512];
	url_encode_rfc3986(signature_hash_buf, signature_hash_encoded_buf, _az_COUNTOF(signature_hash_encoded_buf));

	size_t sas_token_buf_len;
	az_span signature_hash_encoded = az_span_create_from_str(signature_hash_encoded_buf);
	result = az_iot_hub_client_sas_get_password(&hub_client, signature_hash_encoded, expire_time, AZ_SPAN_NULL, sas_token_buf, sizeof(sas_token_buf), &sas_token_buf_len);
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_sas_get_password failed");
		return;
	}

	mqttConnectPacket cloudConnectPacket;
	memset(&cloudConnectPacket, 0, sizeof(mqttConnectPacket));
	cloudConnectPacket.connectVariableHeader.connectFlagsByte.All = 0x20; // AZ_CLIENT_DEFAULT_MQTT_CONNECT_CLEAN_SESSION
	cloudConnectPacket.connectVariableHeader.keepAliveTimer = AZ_IOT_DEFAULT_MQTT_CONNECT_KEEPALIVE_SECONDS;

	cloudConnectPacket.clientID = az_span_ptr(device_id);
	cloudConnectPacket.password = (uint8_t*)sas_token_buf;
	cloudConnectPacket.passwordLength = sas_token_buf_len;
	cloudConnectPacket.username = (uint8_t*)username_buf;
	cloudConnectPacket.usernameLength = (uint16_t)username_buf_len;

	MQTT_CreateConnectPacket(&cloudConnectPacket);
}

bool MQTT_CLIENT_iothub_subscribe()
{
	mqttSubscribePacket cloudSubscribePacket;
	// Variable header   
	cloudSubscribePacket.packetIdentifierLSB = 1;
	cloudSubscribePacket.packetIdentifierMSB = 0;

	cloudSubscribePacket.subscribePayload[0].topic = (uint8_t*) AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC;
	cloudSubscribePacket.subscribePayload[0].topicLength = sizeof(AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC) - 1;
	cloudSubscribePacket.subscribePayload[0].requestedQoS = 0;
	cloudSubscribePacket.subscribePayload[1].topic = (uint8_t*) AZ_IOT_HUB_CLIENT_METHODS_SUBSCRIBE_TOPIC;
	cloudSubscribePacket.subscribePayload[1].topicLength = sizeof(AZ_IOT_HUB_CLIENT_METHODS_SUBSCRIBE_TOPIC) - 1;
	cloudSubscribePacket.subscribePayload[1].requestedQoS = 0;
	cloudSubscribePacket.subscribePayload[2].topic = (uint8_t*) AZ_IOT_HUB_CLIENT_TWIN_PATCH_SUBSCRIBE_TOPIC;
	cloudSubscribePacket.subscribePayload[2].topicLength = sizeof(AZ_IOT_HUB_CLIENT_TWIN_PATCH_SUBSCRIBE_TOPIC) - 1;
	cloudSubscribePacket.subscribePayload[2].requestedQoS = 0;
	cloudSubscribePacket.subscribePayload[3].topic = (uint8_t*) AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_SUBSCRIBE_TOPIC;
	cloudSubscribePacket.subscribePayload[3].topicLength = sizeof(AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_SUBSCRIBE_TOPIC) - 1;
	cloudSubscribePacket.subscribePayload[3].requestedQoS = 0;

	imqtt_publishReceiveCallBackTable[0].topic = AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC;
//	imqtt_publishReceiveCallBackTable[0].mqttHandlePublishDataCallBack = receivedFromCloud_c2d;
	imqtt_publishReceiveCallBackTable[1].topic = AZ_IOT_HUB_CLIENT_METHODS_SUBSCRIBE_TOPIC;
//	imqtt_publishReceiveCallBackTable[1].mqttHandlePublishDataCallBack = receivedFromCloud_message;
	imqtt_publishReceiveCallBackTable[2].topic = AZ_IOT_HUB_CLIENT_TWIN_PATCH_SUBSCRIBE_TOPIC;
//	imqtt_publishReceiveCallBackTable[2].mqttHandlePublishDataCallBack = receivedFromCloud_patch;
	imqtt_publishReceiveCallBackTable[3].topic = AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_SUBSCRIBE_TOPIC;
//	imqtt_publishReceiveCallBackTable[3].mqttHandlePublishDataCallBack = receivedFromCloud_twin;
	MQTT_SetPublishReceptionHandlerTable(imqtt_publishReceiveCallBackTable);

	bool ret = MQTT_CreateSubscribePacket(&cloudSubscribePacket);
	if (ret == true)
	{
		debug_printInfo("MQTT: SUBSCRIBE packet created");
	}

	return ret;
}

void MQTT_CLIENT_iothub_connected()
{
	// get the current state of the device twin

	az_result result = az_iot_hub_client_twin_document_get_publish_topic(&hub_client, twin_request_id, mqtt_get_topic_twin_buf, sizeof(mqtt_get_topic_twin_buf), NULL);
	if (az_failed(result))
	{
		debug_printError("az_iot_hub_client_twin_document_get_publish_topic failed");
		return;
	}

	mqttPublishPacket cloudPublishPacket;
	// Fixed header
	cloudPublishPacket.publishHeaderFlags.duplicate = AZ_HUB_CLIENT_DEFAULT_MQTT_TELEMETRY_DUPLICATE;
	cloudPublishPacket.publishHeaderFlags.qos = AZ_HUB_CLIENT_DEFAULT_MQTT_TELEMETRY_QOS;
	cloudPublishPacket.publishHeaderFlags.retain = AZ_HUB_CLIENT_DEFAULT_MQTT_TELEMETRY_RETAIN;
	// Variable header
	cloudPublishPacket.topic = (uint8_t*)mqtt_get_topic_twin_buf;

	// Payload
	cloudPublishPacket.payload = NULL;
	cloudPublishPacket.payloadLength = 0;

	if (MQTT_CreatePublishPacket(&cloudPublishPacket) != true)
	{
		debug_printError("MQTT: Connection lost PUBLISH failed");
	}
}
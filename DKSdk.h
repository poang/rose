#ifndef SDK_H
#define SDK_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif



#define SDK_READER_TYPE_SIZE 6U
#define SDK_READER_ID_SIZE 16U
#define SDK_READER_KEY_PARAMETER_MAX_SIZE 16U
#define SDK_CARD_ID_SIZE 16U
#define SDK_CHANNEL_ID_MAX_SIZE 6U

// =========================================================================
// 超时宏定义配置 (单位: 100ms Tick)
// =========================================================================
#define SDK_TIMEOUT_OVERALL_TICKS 30  // 总认证时间限制 3s 
#define SDK_TIMEOUT_STEP_TICKS    10  // 单步最大耗时限制 1s 

// =========================================================================
// 日志等级控制配置
// =========================================================================
#define SDK_LOG_LEVEL_NONE    0 // 关闭所有日志
#define SDK_LOG_LEVEL_ERROR   1 // 仅开启 Error 日志 (目前错误和超时)
#define SDK_LOG_LEVEL_DEBUG   2 // 开启 Debug 

#define SDK_LOG_LEVEL  SDK_LOG_LEVEL_ERROR 

// Error codes
#define ERR_AUTH_BASE 0x10
#define ERR_AUTH_SELECT_SE_FAIL 0x11
#define ERR_AUTH_SELECT_DK_FAIL 0x12
#define ERR_AUTH_GPD_FAIL 0x13
#define ERR_AUTH_INTERNAL_FAIL 0x14
#define ERR_AUTH_DK_AUTH_FAIL 0x15
#define ERR_AUTH_EXTERNAL_FAIL 0x16
#define ERR_AUTH_NO_PERMISSION 0x17
#define ERR_AUTH_TIMEOUT 0x18
#define ERR_COMMAND_TIMEOUT 0x19
#define ERR_NOT_INIT  0x1A
#define ERR_PARSE_CRC_FAIL 0x21
#define ERR_PARSE_FRAME_TOO_SHORT 0x22
#define ERR_PARSE_SOF_WRONG 0x23
#define ERR_PARSE_MESSAGEID_WRONG 0x24
#define ERR_PARSE_COMMANDID_WRONG 0x25
#define ERR_PARSE_TLV_WRONG 0x26
#define ERR_PARSE_SW_WRONG 0x27

typedef enum
{
    SDK_CHANNEL_TYPE_NFC = 0,
    SDK_CHANNEL_TYPE_BLE = 1
} Sdk_ChannelType;

typedef struct
{
    Sdk_ChannelType channel_type;
    uint8_t protocol_type;
    uint8_t id[SDK_CHANNEL_ID_MAX_SIZE];
    uint8_t idSize;
} Sdk_Channel;



typedef struct
{
    Sdk_Channel channel;
    uint8_t *dataBuffer;
    uint16_t dataSize;
} Sdk_SendParam;

typedef struct
{
    uint8_t *dataBuffer;
    uint16_t dataSize;
} Sdk_ApduParam;

typedef struct
{
    uint8_t rkeResult[8];
} Sdk_RKECallbackParam;



typedef struct
{
    Sdk_Channel channel;
    uint8_t errorCode;
    uint8_t sw[2];
    uint8_t cardId[16];
} Sdk_NotifyAuthResultParam;

typedef struct
{
    Sdk_Channel channel;
    uint8_t *data;
    uint8_t dataSize;
} Sdk_CalibData;

typedef struct
{
    Sdk_Channel channel;
    uint8_t *dataBuffer;
    uint16_t dataSize;
} Sdk_GetCalibReq;

typedef struct
{
    Sdk_Channel channel;
    uint8_t dataBuffer[16];
    uint16_t dataSize;
} Sdk_DataReportParam;

typedef struct
{
	uint8_t SDK_Version[4];
	uint8_t applet_version[4];
	uint8_t SEID[16];
	
}Sdk_VersionParam;

typedef struct 
{
	Sdk_Channel channel;
	uint8_t RKEcmd;
	
}Sdk_RKEParam;

typedef struct
{
    uint8_t *reqBuffer;
    uint16_t reqSize;
} SEAgent_ApduParam;

typedef struct
{
    uint8_t *reqBuffer;
    uint16_t reqSize;
} SEAgent_ExecTsmParam;

typedef uint8_t (*Sdk_Apdu)(Sdk_ApduParam *param);
typedef uint8_t (*Sdk_Send)(Sdk_SendParam *param);
typedef uint8_t (*Sdk_GetVersionCallback)(Sdk_VersionParam *param);
typedef uint8_t (*Sdk_RKE)(Sdk_RKEParam *param);
typedef uint8_t (*Sdk_NotifyAuthResult)(Sdk_NotifyAuthResultParam *param);
typedef uint8_t (*Sdk_NotifyCalibData)(Sdk_CalibData *param);
typedef uint8_t (*Sdk_DataReportCallback)(Sdk_Channel *nChannel, uint8_t *dataBuffer, uint16_t dataSize);
typedef void (*Sdk_GetLogCallback)(uint8_t *logBuffer, uint16_t logSize);

typedef uint8_t (*SEAgent_apdu)(SEAgent_ApduParam *param);
typedef uint8_t (*SEAgent_ApplySeOper)(uint16_t duration);
typedef uint8_t (*SEAgent_ReleaSeOper)(void);
typedef uint8_t (*SEAgent_AppletUpdate)(uint8_t result);

typedef struct
{
    Sdk_Apdu apdu_cb;
    Sdk_Send send_cb;
    Sdk_RKE rke_cb;
    Sdk_NotifyAuthResult notify_auth_cb;
    Sdk_NotifyCalibData notify_calib_cb;
    Sdk_GetLogCallback get_log_cb;
    Sdk_GetVersionCallback get_version_cb;
    Sdk_DataReportCallback data_report_cb;
    SEAgent_apdu seagent_apdu_cb;
    SEAgent_ApplySeOper seagent_apply_cb;
    SEAgent_ReleaSeOper seagent_release_cb;
    SEAgent_AppletUpdate seagent_update_cb;
} Sdk_Config_t;

void Sdk_Init (void);                                                             


void Sdk_routine(void);

uint8_t Sdk_Auth(Sdk_Channel *nChannel);
uint8_t Sdk_CancelAuth(Sdk_Channel *nChannel);
uint8_t Sdk_ApduCallback(uint8_t result, uint8_t *dataBuffer, uint16_t dataSize);
uint8_t Sdk_SendCallback(Sdk_Channel *nChannel, uint8_t result, uint8_t *dataBuffer, uint16_t dataSize);
uint8_t Sdk_RKECallback(Sdk_Channel *nChannel, Sdk_RKECallbackParam *param);
uint8_t Sdk_GetLog(uint16_t bufferMaxSize);
uint8_t Sdk_GetCalibData(Sdk_GetCalibReq *req);
uint8_t Sdk_DataReport(Sdk_DataReportParam *param);
uint8_t SEAgent_ApduCallback(uint8_t result,  uint8_t *respData, uint16_t dataSize);
uint8_t SEAgent_ExecTsmCmd(SEAgent_ExecTsmParam  *param);
void Sdk_Release(Sdk_Channel *nChannel);

void Sdk_RegisterApdu(Sdk_Apdu apduFunc);
void Sdk_RegisterSend(Sdk_Send sendFunc);
void Sdk_RegisterRKE(Sdk_RKE rkeFunc);
void Sdk_RegisterGetLogCallback(Sdk_GetLogCallback getLogFunc);
void Sdk_RegisterNotifyAuthResult(Sdk_NotifyAuthResult notifyAuthResultFunc);
void Sdk_RegisterGetVersionCallback(Sdk_GetVersionCallback getVersionFunc);
void Sdk_RegisterNotifyCalibData(Sdk_NotifyCalibData notifyCalibFunc);
void Sdk_RegisterDataReportCallback(Sdk_DataReportCallback dataReportFunc);

#ifdef __cplusplus
}
#endif

#endif // SDK_H
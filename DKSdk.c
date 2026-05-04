#include "DKSdk.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_APDU_COMMAND_LENGTH 260
#define STATUS_SUCCESS 0
#define STATUS_FAILED 1

// BLE frame header
#define ICCE_SOF 0x5A
#define MESSAGE_TYPE_AUTH 0x01
#define MESSAGE_TYPE_COMMAND 0x02
#define MESSAGE_TYPE_NOTIFICATION 0x03

#define COMMAND_TYPE_AUTH 0x01
#define COMMAND_TYPE_CMD 0x02
#define COMMAND_TYPE_RKE 0x03
#define COMMAND_TYPE_GETINFO 0x06

// 状态机常量
#define STATE_MACHINE_INIT 0x00
#define STATE_MACHINE_SELECT_SE 0x01
#define STATE_MACHINE_SELECT_DK 0x02
#define STATE_MACHINE_GPD 0x03
#define STATE_MACHINE_INTERNAL_AUTH 0x04
#define STATE_MACHINE_AUTH 0x05
#define STATE_MACHINE_EXTERNAL_AUTH 0x06
#define STATE_MACHINE_DATAREPORT_REQUEST 0x08
#define STATE_MACHINE_DATAREPORT_RESPONSE 0x09
#define STATE_MACHINE_RKE_REQUEST 0x0A
#define STATE_MACHINE_RKE_RESPONSE 0x0B
#define STATE_MACHINE_CALIBDATA 0x0E
#define STATE_MACHINE_OFFLINE_AUTH 0x0F

#define SDK_MAX_SESSIONS 6
#define SDK_MAX_HW_QUEUE 6

// Sdk_routine 超时阈值（单位：Sdk_routine 调用次数）
// 建议 MCU 以 1ms 周期调用 Sdk_routine，以下阈值对应：
//   SE_CMD_TIMEOUT_TICKS  = 1000 → SE单指令超时 1秒
//   SESSION_TIMEOUT_TICKS = 3000 → 会话总超时 3秒
//   PEER_STEP_TIMEOUT_TICKS= 1000 → 等对端回调超时 1秒
// 如果 MCU 改变调用周期，只需修改这三个值
#define SE_CMD_TIMEOUT_TICKS 1000
#define SESSION_TIMEOUT_TICKS 3000
#define PEER_STEP_TIMEOUT_TICKS 1000

// =========================================================================
// 结构体定义
// =========================================================================

/*
 *@brief ICCE帧信息结构体
 *@param control_field 控制字段
 *@param is_request 是否为请求
 *@param message_id 消息ID
 *@param command_id 命令ID
 *@param fsn 帧序列号
 *@param payload 帧数据
 *@param payload_length 帧数据长度
 */
typedef struct
{
    uint8_t control_field;
    uint8_t is_request;
    uint8_t message_id;
    uint8_t command_id;
    uint8_t fsn;
    const uint8_t *payload;
    uint16_t payload_length;
} ICCE_FrameInfo_t;
/*
 *@brief 会话上下文结构体
 *@param is_active 是否激活
 *@param channel 通道
 *@param status 状态
 *@param overall_ticks 当前会话总超时时长
 *@param peer_step_ticks 每条指令的超时时长
 *@param is_waiting_peer 是否等待回调
 *@param card_id 卡片ID
 *@param status_word 状态字SW
 *@param apdu_resp SE APDU响应缓冲区（每会话独立）
 *@param apdu_resp_length SE APDU响应长度
 *@param dk_apdu_resp DK/BLE响应缓冲区（每会话独立）
 *@param dk_apdu_resp_length DK/BLE响应长度
 *@param tlv_buffer TLV构建缓冲区（每会话独立）
 *@param parsed_buffer 组帧缓冲区（每会话独立）
 *@param parsed_buffer_length 组帧数据长度
 *@param reader_rnd 读卡器随机数（每会话独立）
 *@param reader_key_parameter 读卡器密钥参数（每会话独立）
 */
typedef struct
{
    uint8_t is_active;
    Sdk_Channel channel;
    uint8_t status;
    uint16_t overall_ticks; // 当前会话总超时时长

    uint16_t peer_step_ticks; // 每条指令的超时时长
    uint8_t is_waiting_peer;  // 是否等待回调

    // 每个通道管理一组认证参数
    uint8_t card_id[16];

    // 状态字SW
    uint16_t status_word;

    // === 每会话独立的响应缓冲区（解决多连接全局变量冲突） ===
    uint8_t apdu_resp[MAX_APDU_COMMAND_LENGTH];
    uint16_t apdu_resp_length;
    uint8_t dk_apdu_resp[MAX_APDU_COMMAND_LENGTH];
    uint16_t dk_apdu_resp_length;

    // === 每会话独立的工作缓冲区 ===
    uint8_t tlv_buffer[256];
    uint8_t parsed_buffer[MAX_APDU_COMMAND_LENGTH];
    uint16_t parsed_buffer_length;

    // === 每会话独立的认证参数（防止多连接同时认证时互相覆盖） ===
    uint8_t reader_rnd[8];
    uint8_t reader_key_parameter[16];

    // === RKE 延迟队列（解决 DataReport 占用状态机时 RKE 无法执行的问题） ===
    // 当通道正在执行 DataReport/认证时，RKE 数据先暂存于此，
    // 等当前流程结束后再自动取出执行，确保两个指令都能正常完成
    uint8_t pending_rke_data[MAX_APDU_COMMAND_LENGTH];
    uint16_t pending_rke_length;
    uint8_t has_pending_rke; // 0=无pending, 1=有pending RKE待处理
} Sdk_SessionContext;

/*
 *@brief SE APDU命令结构体
 *@param owner 会话所有者
 *@param expected_status 入队时期望的会话状态（用于检测会话是否被重新分配）
 *@param apdu APDU命令
 *@param apdu_len APDU命令长度
 */
typedef struct
{
    Sdk_SessionContext *owner; // 保留当前会话所有者
    uint8_t expected_status;   // 入队时的会话状态，用于防重入校验
    uint8_t apdu[MAX_APDU_COMMAND_LENGTH];
    uint16_t apdu_len;
} Sdk_HwApduCmd;

/*
 *@brief SE APDU命令管理器结构体
 *@param queue APDU命令队列
 *@param head 队列头
 *@param tail 队列尾
 *@param count 队列数量
 *@param is_hw_busy 是否忙碌
 *@param queue_locked 队列操作锁（防止入队/出队并发冲突）
 *@param hw_step_ticks SE指令响应时间
 */
typedef struct
{
    Sdk_HwApduCmd queue[SDK_MAX_HW_QUEUE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t is_hw_busy;
    uint8_t queue_locked;   // 队列操作锁：0=未锁定, 1=已锁定
    uint16_t hw_step_ticks; // SE指令响应时间

    // === ISR → 主循环 响应转发缓冲区 ===
    // ISR 只写这些字段（不碰 head/tail/count），主循环轮询消费
    volatile uint8_t apdu_response_ready; // ISR置1，主循环处理后清0
    uint8_t apdu_response_result;
    uint8_t apdu_response_data[MAX_APDU_COMMAND_LENGTH];
    uint16_t apdu_response_size;
} Sdk_HwApduManager;

// ========================================================================
// 全局状态（仅保留真正全局的共享数据）
//=========================================================================
Sdk_Config_t g_sdk_cbs = {0};
Sdk_NotifyAuthResultParam NotifyAuthResultParam;
Sdk_VersionParam versionParam;
Sdk_RKEParam rkeParam;
static Sdk_SessionContext g_Sessions[SDK_MAX_SESSIONS];
static Sdk_HwApduManager g_HwMgr = {0};

// 全局日志缓冲区（仅用于日志输出，不参与业务逻辑）
static uint8_t logbuf[512];
static uint16_t logbuf_len = 0;
static uint8_t applet_version[4];
static uint8_t SEID[16];
static uint8_t sdk_version[4] = {0x26, 0x04, 0x29, 0x01}; // SDK 版本

static uint8_t g_reader_id[8] = {0x20, 0x43, 0x88, 0x20, 0x24, 0x98, 0x43, 0x79}; // TODO 暂时为固定值

// --- 队列操作锁辅助方法 ---
/*
 *@brief 尝试获取队列锁
 *@return 1: 获取成功, 0: 队列正被占用
 */
static uint8_t Sdk_HwQueue_TryLock(void)
{
    if (g_HwMgr.queue_locked)
    {
        return 0;
    }
    g_HwMgr.queue_locked = 1;
    return 1;
}

/*
 *@brief 释放队列锁
 */
static void Sdk_HwQueue_Unlock(void)
{
    g_HwMgr.queue_locked = 0;
}

// --- 接口声明 ---
static uint8_t Sdk_IsChannelEqual(Sdk_Channel *c1, Sdk_Channel *c2);
static void Sdk_LogError(char *format, ...);
static void Sdk_ReportResult(Sdk_NotifyAuthResultParam *param);
static void Sdk_NotifyBLEResult(Sdk_SessionContext *sess, uint8_t type, uint8_t len, uint8_t value);
static void Sdk_FinishSession(Sdk_SessionContext *session);
static void Trigger_Hw_Send(void);
static void Sdk_ProcessPendingRKE(Sdk_SessionContext *sess);
static void Sdk_ProcessApduResponse(void);

// =========================================================================
// 静态方法
// =========================================================================

/*
 *@brief 判断通道是否相等
 *@param c1 通道1
 *@param c2 通道2
 *@return 1: 相等, 0: 不相等
 */
static uint8_t Sdk_IsChannelEqual(Sdk_Channel *c1, Sdk_Channel *c2)
{
    if (c1 == NULL || c2 == NULL)
    {
        return 0;
    }
    if (c1->channel_type != c2->channel_type)
    {
        return 0;
    }
    if (c1->idSize != c2->idSize)
    {
        return 0;
    }
    if (c1->idSize > 0 && memcmp(c1->id, c2->id, c1->idSize) != 0)
    {
        return 0;
    }
    return 1;
}

/*
 *@brief 打印错误日志
 *@param format 格式化字符串
 *@param ... 可变参数
 */
static void Sdk_LogError(char *format, ...)
{
#if SDK_LOG_LEVEL >= SDK_LOG_LEVEL_DEBUG
    va_list args;
    va_start(args, format);
    logbuf_len = vsprintf((char *)logbuf, format, args);
    logbuf[logbuf_len] = '\0';
    va_end(args);

    if (g_sdk_cbs.get_log_cb != NULL)
    {
        g_sdk_cbs.get_log_cb(logbuf, logbuf_len);
    }
#endif
}

/*
 *@brief 打印调试日志
 *@param format 格式化字符串
 *@param ... 可变参数
 */
static void Sdk_LogDebug(char *format, ...)
{
#if SDK_LOG_LEVEL >= SDK_LOG_LEVEL_DEBUG
    va_list args;
    va_start(args, format);
    logbuf_len = vsprintf((char *)logbuf, format, args);
    logbuf[logbuf_len] = '\0';
    va_end(args);

    if (g_sdk_cbs.get_log_cb != NULL)
    {
        g_sdk_cbs.get_log_cb(logbuf, logbuf_len);
    }
#endif
}

/*
 *@brief 打印调试日志
 *@param prefix 日志前缀
 *@param data 日志数据
 *@param len 日志数据长度
 */
static void Sdk_LogDebugHex(const char *prefix, const uint8_t *data, uint16_t len)
{
#if SDK_LOG_LEVEL >= SDK_LOG_LEVEL_DEBUG
    uint16_t i;
    if (g_sdk_cbs.get_log_cb == NULL || len == 0)
    {
        return;
    }
    logbuf_len = sprintf((char *)logbuf, "%s [%d Bytes]: ", prefix, len);
    for (i = 0; i < len && logbuf_len < sizeof(logbuf) - 4; i++)
    {
        logbuf_len += sprintf((char *)&logbuf[logbuf_len], "%02X", data[i]);
        if (logbuf_len >= 150)
        {
            logbuf_len += sprintf((char *)&logbuf[logbuf_len], "\r\n");
            g_sdk_cbs.get_log_cb(logbuf, logbuf_len);
            logbuf_len = 0;
        }
    }
    if (logbuf_len > 0)
    {
        logbuf_len += sprintf((char *)&logbuf[logbuf_len], "\r\n");
        g_sdk_cbs.get_log_cb(logbuf, logbuf_len);
    }
#endif
}

/*
 *@brief 报告认证结果
 *@param param 认证结果参数
 */
static void Sdk_ReportResult(Sdk_NotifyAuthResultParam *param)
{
    Sdk_LogError("ErrorCode: %02X, SW: 0x%02X%02X", param->errorCode, param->sw[0], param->sw[1]);
    if (g_sdk_cbs.notify_auth_cb != NULL)
    {
        g_sdk_cbs.notify_auth_cb(param);
    }
}

/*
 *@brief 获取版本信息
 *@return 1: 成功, 0: 失败
 */
static void Sdk_GetVersionInfo()
{

    memcpy(versionParam.SDK_Version, sdk_version, 4);

    if (g_sdk_cbs.get_version_cb != NULL)
    {
        g_sdk_cbs.get_version_cb(&versionParam);
    }
    else
    {
        Sdk_LogError("Sdk_Get_VersionCallback is not register!");
    }
}

// ========================================================================
// ICCE 蓝牙帧解析
// ========================================================================

/*
 *@brief 计算CRC16校验
 *@param data 数据
 *@param length 数据长度
 *@return CRC16校验值
 */
static uint16_t icce_crc16_false(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    int i, j;
    for (i = 0; i < length; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint8_t padding_data(uint8_t *data, int in_len)
{
    uint8_t total_len;
    data[in_len] = 0x80;

    total_len = in_len + 1;
    int pad_len = (16 - (total_len % 16)) % 16;

    // 第三步：补 0x00
    memset(data + total_len, 0x00, pad_len);

    // 最终长度
    return total_len + pad_len;
}

/*
 *@brief 解析SW状态码
 *@param sess 会话上下文
 *@param sw_buffer SW缓冲区
 *@param sw_buffer_size SW缓冲区大小
 *@return SW状态码
 */
static uint16_t parsingSW(Sdk_SessionContext *sess, uint8_t *sw_buffer, uint16_t *sw_buffer_size)
{
    if (*sw_buffer_size < 2)
    {
        return 0xFFFF;
    }
    sess->status_word = ((uint16_t)sw_buffer[*sw_buffer_size - 2] << 8) | sw_buffer[*sw_buffer_size - 1];
    (*sw_buffer_size) -= 2;
    return sess->status_word;
}

/*
 *@brief 构建TLV数据
 *@param tag TLV标签
 *@param inputBuffer 输入缓冲区
 *@param input_length 输入长度
 *@param outputBuffer 输出缓冲区
 *@return TLV长度
 */
static uint16_t buildTLV(uint8_t tag, const uint8_t *inputBuffer, uint16_t input_length, uint8_t *outputBuffer)
{
    uint16_t output_length = 0;
    uint8_t offset = 0;
    outputBuffer[offset++] = tag;
    if (input_length >= 0x80)
    {
        outputBuffer[offset++] = 0x81;
        outputBuffer[offset++] = input_length;
        output_length += 3;
    }
    else
    {
        outputBuffer[offset++] = input_length;
        output_length += 2;
    }

    if (input_length > 0 && inputBuffer != NULL)
    {
        memcpy(outputBuffer + output_length, inputBuffer, input_length);
    }

    return output_length + input_length;
}

/*
 *@brief 添加TLV数据
 *@param t TLV标签
 *@param tag_len TLV标签长度
 *@param tag_data TLV数据
 *@param cmd 命令缓冲区
 *@param cmd_len 命令长度
 *@return TLV长度
 */
static uint16_t apducommand_addtag(uint8_t t[2], uint8_t tag_len, uint8_t *tag_data, uint8_t *cmd, uint16_t cmd_len)
{
    memcpy(cmd + cmd_len, t, 2);
    *(cmd + cmd_len + 2) = tag_len;
    memcpy(cmd + cmd_len + 3, tag_data, tag_len);
    return (cmd_len + 3 + tag_len);
}

/*
 *@brief 构建帧数据
 *@param output_buffer 输出缓冲区
 *@param message_id 消息ID
 *@param command_id 命令ID
 *@param control 控制字段
 *@param payload 帧数据
 *@param input_length 帧数据长度
 *@return 帧长度
 */
static uint16_t build_frame_from_se_apdu(uint8_t *output_buffer, uint8_t message_id, uint8_t command_id,
                                         uint8_t control, const uint8_t *payload, uint16_t input_length)
{
    uint16_t output_length;
    uint16_t frame_length;
    uint16_t crc;
    uint8_t *ptr = output_buffer;
    output_length = 1 + 2 + 1 + 1 + 1 + 1 + input_length + 2;
    frame_length = 1 + 1 + 1 + 1 + input_length;

    *ptr++ = ICCE_SOF;
    *ptr++ = frame_length & 0xFF;
    *ptr++ = (frame_length >> 8) & 0xFF;
    *ptr++ = control;
    *ptr++ = 0x00;
    *ptr++ = message_id;
    *ptr++ = command_id;
    if (input_length > 0)
    {
        memcpy(ptr, payload, input_length);
        ptr += input_length;
    }
    crc = icce_crc16_false(&output_buffer[0], output_length - 2);
    *ptr++ = crc & 0xFF;
    *ptr++ = (crc >> 8) & 0xFF;
    return output_length;
}

/*
 *@brief 解析TLV数据
 *@param tlvinputTag TLV标签
 *@param tlvinputbuffer TLV输入缓冲区
 *@param tlvinputTotal TLV输入总长度
 *@param tlvoutputbuffer TLV输出缓冲区
 *@param outputbufferSize TLV输出缓冲区大小
 *@return TLV长度
 */
static uint16_t tlvdata_parsing(uint8_t tlvinputTag[2], uint8_t *tlvinputbuffer, uint16_t tlvinputTotal,
                                uint8_t *tlvoutputbuffer, uint16_t outputbufferSize)
{
    uint16_t i = 0;
    uint16_t n = 0;
    uint8_t tag_count = 0;
    uint8_t tag_startpos = 0;

    if (outputbufferSize == 2)
    {
        memset(tlvoutputbuffer, 0x00, 2);
    }
    if (tlvinputTotal > MAX_APDU_COMMAND_LENGTH)
    {
        return 0;
    }

    if (tlvinputbuffer[0] == 0x77)
    {
        if (tlvinputbuffer[1] == 0x81)
        {
            tag_startpos = 3;
        }
        else
        {
            tag_startpos = 2;
        }
    }

    for (i = tag_startpos; i < tlvinputTotal;)
    {
        if (tlvinputbuffer[i] == tlvinputTag[0])
        {
            if ((tlvinputbuffer[i] & 0x1F) == 0x1F)
            {
                if (tlvinputbuffer[i + 1] == tlvinputTag[1])
                {
                    if (tlvinputbuffer[i + 2] == 0x81)
                    {
                        n = tlvinputbuffer[i + 3];
                        if (outputbufferSize == 2)
                        {
                            *tlvoutputbuffer = (uint8_t)((i + 4) / 256);
                            *(tlvoutputbuffer + 1) = (uint8_t)(i + 4);
                            return n;
                        }
                        else if (n <= outputbufferSize)
                        {
                            memcpy(tlvoutputbuffer, &tlvinputbuffer[i + 4], n);
                            return n;
                        }
                        else
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        n = tlvinputbuffer[i + 2];
                        if (outputbufferSize == 2)
                        {
                            *tlvoutputbuffer = (uint8_t)((i + 3) / 256);
                            *(tlvoutputbuffer + 1) = (uint8_t)(i + 3);
                            return n;
                        }
                        else if (n <= outputbufferSize)
                        {
                            memcpy(tlvoutputbuffer, &tlvinputbuffer[i + 3], n);
                            return n;
                        }
                        else
                        {
                            return 0;
                        }
                    }
                }
                else
                {
                    if (tlvinputbuffer[i + 2] == 0x81)
                    {
                        i = i + tlvinputbuffer[i + 3] + 4;
                    }
                    else
                    {
                        i = i + tlvinputbuffer[i + 2] + 3;
                    }
                }
            }
            else
            {
                if (tlvinputbuffer[i + 1] == 0x81)
                {
                    n = tlvinputbuffer[i + 2];
                    if (outputbufferSize == 2)
                    {
                        *tlvoutputbuffer = (uint8_t)((i + 3) / 256);
                        *(tlvoutputbuffer + 1) = (uint8_t)(i + 3);
                        return n;
                    }
                    else if (n <= outputbufferSize)
                    {
                        memcpy(tlvoutputbuffer, &tlvinputbuffer[i + 3], n);
                        return n;
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    n = tlvinputbuffer[i + 1];
                    if (outputbufferSize == 2)
                    {
                        *tlvoutputbuffer = (uint8_t)((i + 2) / 256);
                        *(tlvoutputbuffer + 1) = (uint8_t)(i + 2);
                        return n;
                    }
                    else if (n <= outputbufferSize)
                    {
                        memcpy(tlvoutputbuffer, &tlvinputbuffer[i + 2], n);
                        return n;
                    }
                    else
                    {
                        return 0;
                    }
                }
            }
        }
        else
        {
            if ((tlvinputbuffer[i] & 0x1F) == 0x1F)
            {
                if (tlvinputbuffer[i + 2] == 0x81)
                {
                    i = i + tlvinputbuffer[i + 3] + 4;
                }
                else
                {
                    i = i + tlvinputbuffer[i + 2] + 3;
                }
            }
            else
            {
                if (tlvinputbuffer[i + 1] == 0x81)
                {
                    i = i + tlvinputbuffer[i + 2] + 3;
                }
                else
                {
                    i = i + tlvinputbuffer[i + 1] + 2;
                }
            }
        }
        tag_count++;
        if (tag_count > 32)
        {
            break;
        }
    }
    return 0;
}

/*
 *@brief 解析帧数据
 *@param nChannel 通道
 *@param ble_buffer 蓝牙缓冲区
 *@param buffer_size 缓冲区大小
 *@param frame_info 帧信据
 *@return 1: 成功, 0: 失败
 */
static uint8_t icce_parse_frame(Sdk_Channel *nChannel, const uint8_t *ble_buffer, uint16_t buffer_size,
                                ICCE_FrameInfo_t *frame_info)
{
    const uint8_t *crc_start;
    uint16_t Calculated_crc, Received_crc, crc_length;
    uint16_t frame_length, total_frame_length;
    uint16_t raw_payload_len; // 可能包含status tlv
    const uint8_t *status_tlv;
    uint8_t status_tlv_len;
    uint8_t status_value;
    Sdk_NotifyAuthResultParam localNotify; // 栈变量，防止重入覆盖

    if (buffer_size < 9)
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_FRAME_TOO_SHORT;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }

    crc_start = &ble_buffer[0];
    crc_length = buffer_size - 2;
    Calculated_crc = icce_crc16_false(crc_start, crc_length);
    Received_crc = ((uint16_t)ble_buffer[buffer_size - 1] << 8 | ble_buffer[buffer_size - 2]);
    if (Calculated_crc != Received_crc)
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_CRC_FAIL;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }
    if (ble_buffer[0] != ICCE_SOF)
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_SOF_WRONG;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }

    frame_length = (ble_buffer[1]) | (uint16_t)(ble_buffer[2] << 8);
    total_frame_length = frame_length + 5;

    if (buffer_size < total_frame_length)
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_TLV_WRONG;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }

    frame_info->control_field = ble_buffer[3];
    frame_info->fsn = ble_buffer[4];
    frame_info->message_id = ble_buffer[5];
    frame_info->command_id = ble_buffer[6];
    frame_info->is_request = frame_info->control_field & 0x10;

    if ((frame_info->message_id != 0x01) && (frame_info->message_id != 0x02) && (frame_info->message_id != 0x03))
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_MESSAGEID_WRONG;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }

    if ((frame_info->command_id != 0x01) && (frame_info->command_id != 0x02) && (frame_info->command_id != 0x03) &&
        (frame_info->command_id != 0x04))
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_COMMANDID_WRONG;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }

    if (frame_length < 6)
    {
        localNotify.channel = *nChannel;
        localNotify.errorCode = ERR_PARSE_TLV_WRONG;
        localNotify.sw[0] = 0;
        localNotify.sw[1] = 0;
        Sdk_ReportResult(&localNotify);
        return 0x01;
    }

    raw_payload_len = frame_length - 4;

    switch (frame_info->message_id)
    {
    case 0x01: // AUTH
        if (frame_info->fsn == 0x00)
        {
            status_tlv = ble_buffer + 7;
            if ((status_tlv[1] & 0x7F) != 0x01)
            {
                localNotify.channel = *nChannel;
                localNotify.errorCode = ERR_PARSE_TLV_WRONG;
                localNotify.sw[0] = 0;
                localNotify.sw[1] = 0;
                Sdk_ReportResult(&localNotify);
                return 0x01;
            }
            status_value = status_tlv[2];
            if (status_value != 0)
            {
                localNotify.channel = *nChannel;
                localNotify.errorCode = 0x30 + status_value;
                localNotify.sw[0] = 0;
                localNotify.sw[1] = 0;
                Sdk_ReportResult(&localNotify);
                return 0x01;
            }

            status_tlv_len = 2 + (status_tlv[1] & 0x7F); // payload offset
            // TODO need to fix
            if (status_tlv[3] == 0x01)
            { // payload type
                frame_info->payload = status_tlv + status_tlv_len + 8;
                frame_info->payload_length = raw_payload_len - status_tlv_len - 8;
            }
            else
            {
                if (status_tlv[status_tlv_len + 1] <= 0x7F)
                {
                    frame_info->payload = status_tlv + status_tlv_len + 2;
                    frame_info->payload_length = raw_payload_len - status_tlv_len - 2;
                }
                else if (status_tlv[status_tlv_len + 1] == 0x81)
                {
                    frame_info->payload = status_tlv + status_tlv_len + 3;
                    frame_info->payload_length = raw_payload_len - status_tlv_len - 3;
                }
                else if (status_tlv[status_tlv_len + 1] == 0x82)
                {

                    frame_info->payload = status_tlv + status_tlv_len + 4;
                    frame_info->payload_length = raw_payload_len - status_tlv_len - 4;
                }
            }
        }
        else
        {
            frame_info->payload = ble_buffer + 7;
            frame_info->payload_length = raw_payload_len;
        }

        break;

    case 0x02: // RKE

        status_tlv = ble_buffer + 7;
        //                if ((status_tlv[1] & 0x7F) != 1) {
        //                    return 0x01;
        //                }
        //

        frame_info->payload = status_tlv;             // rke value
        frame_info->payload_length = raw_payload_len; // rke length

        break;

    case 0x03: // NOTIFICATION
        Sdk_LogError("NOTIFICATION!");
        status_tlv = ble_buffer + 7;
        if ((status_tlv[1] & 0x7F) != 1)
        {
            return 0x01;
        }
        status_value = status_tlv[2];
        if (status_value != 0)
        {
            localNotify.channel = *nChannel;
            localNotify.errorCode = 0x30 + status_value;
            Sdk_ReportResult(&localNotify);
            return 0x01;
        }
        status_tlv_len = 2 + (status_tlv[1] & 0x7F);
        Sdk_LogError("responese data length%d ", status_tlv + status_tlv_len + 2);
        frame_info->payload = status_tlv + status_tlv_len + 2;
        frame_info->payload_length = raw_payload_len - status_tlv_len - 2;
        break;
    }
    return 0; // Success
}

/*
 *@brief 转换GPD响应
 *@param sess 会话上下文
 */
static void Transform_GPD_Response(Sdk_SessionContext *sess)
{
    uint8_t *p;
    uint16_t len;
    uint8_t *value_5A = NULL, *value_9F3B = NULL, *value_9F3E = NULL, *value_9F05 = NULL, *value_73 = NULL;
    uint16_t len_5A = 0, len_9F3B = 0, len_9F3E = 0, len_9F05 = 0, len_73 = 0;
    uint8_t *curr;
    int remaining;

    uint16_t val_len;
    uint8_t len_bytes;
    uint8_t *current_value_start;
    uint16_t tag_val;
    uint8_t tag_bytes;
    uint16_t total_value_len;

    uint8_t temp_buf[MAX_APDU_COMMAND_LENGTH];
    uint16_t temp_len = 0;

    if (sess->dk_apdu_resp_length < 2)
    {
        return;
    }

    p = sess->dk_apdu_resp;
    len = sess->dk_apdu_resp_length;

    if (p[0] == 0x77)
    {
        val_len = p[1];
        len_bytes = 1;
        if (val_len == 0x81)
        {
            len_bytes = 2;
        }
        else if (val_len == 0x82)
        {
            len_bytes = 3;
        }
        p += (1 + len_bytes);
        len -= (1 + len_bytes);
    }

    curr = p;
    remaining = len;

    while (remaining > 0)
    {
        current_value_start = curr;
        tag_val = 0;
        tag_bytes = 1;

        if ((*curr & 0x1F) == 0x1F)
        {
            if (remaining < 2)
            {
                break;
            }
            tag_val = (*curr << 8) | *(curr + 1);
            tag_bytes = 2;
        }
        else
        {
            tag_val = *curr;
        }
        curr += tag_bytes;
        remaining -= tag_bytes;

        if (remaining <= 0)
        {
            break;
        }

        val_len = *curr;
        len_bytes = 1;
        if (val_len == 0x81)
        {
            if (remaining < 2)
            {
                break;
            }
            val_len = *(curr + 1);
            len_bytes = 2;
        }
        else if (val_len == 0x82)
        {
            if (remaining < 3)
            {
                break;
            }
            val_len = (*(curr + 1) << 8) | *(curr + 2);
            len_bytes = 3;
        }
        curr += len_bytes;
        remaining -= len_bytes;

        total_value_len = tag_bytes + len_bytes + val_len;

        if (tag_val == 0x5A)
        {
            value_5A = current_value_start;
            len_5A = total_value_len;
        }
        else if (tag_val == 0x9F3B)
        {
            value_9F3B = current_value_start;
            len_9F3B = total_value_len;
        }
        else if (tag_val == 0x9F3E)
        {
            value_9F3E = current_value_start;
            len_9F3E = total_value_len;
        }
        else if (tag_val == 0x9F05)
        {
            value_9F05 = current_value_start;
            len_9F05 = total_value_len;
        }
        else if (tag_val == 0x73)
        {
            value_73 = current_value_start;
            len_73 = total_value_len;
        }

        curr += val_len;
        remaining -= val_len;
    }

    if (value_5A)
    {
        memcpy(temp_buf + temp_len, value_5A, len_5A);
        temp_len += len_5A;
    }
    if (value_9F3E)
    {
        memcpy(temp_buf + temp_len, value_9F3E, len_9F3E);
        temp_len += len_9F3E;
    }
    if (value_9F3B)
    {
        memcpy(temp_buf + temp_len, value_9F3B, len_9F3B);
        temp_len += len_9F3B;
    }
    if (value_9F05)
    {
        memcpy(temp_buf + temp_len, value_9F05, len_9F05);
        temp_len += len_9F05;
    }
    if (value_73)
    {
        memcpy(temp_buf + temp_len, value_73, len_73);
        temp_len += len_73;
    }

    memcpy(sess->dk_apdu_resp, temp_buf, temp_len);
    sess->dk_apdu_resp_length = temp_len;
}

// ========================================================================
// ICCE 组包业务调用 Session Context
// ========================================================================

/*
 *@brief 入队APDU命令（带队列锁保护，防止多连接并发入队/出队冲突）
 *@param sess 会话上下文
 *@param apdu APDU命令
 *@param length APDU命令长度
 */
static void internal_apdu_enqueue(Sdk_SessionContext *sess, uint8_t *apdu, uint16_t length)
{
    uint8_t tail;
    uint8_t retry = 0;

    // 等待获取队列锁（最多重试100次，防止死等）
    while (!Sdk_HwQueue_TryLock())
    {
        retry++;
        if (retry > 100)
        {
            Sdk_LogError("enqueue lock timeout!");
            return;
        }
    }

    if (g_HwMgr.count >= SDK_MAX_HW_QUEUE)
    {
        Sdk_LogError("count:%d", g_HwMgr.count);
        Sdk_HwQueue_Unlock();
        return;
    }

    tail = g_HwMgr.tail;
    g_HwMgr.queue[tail].owner = sess;
    g_HwMgr.queue[tail].expected_status = sess->status; // 记录入队时的状态，用于出队校验
    g_HwMgr.queue[tail].apdu_len = length;
    memcpy(g_HwMgr.queue[tail].apdu, apdu, length);
    g_HwMgr.tail = (tail + 1) % SDK_MAX_HW_QUEUE;
    g_HwMgr.count++;

    Sdk_HwQueue_Unlock();
}

/*
 *@brief 发送DK数据
 *@param sess 会话上下文
 *@param payload 数据
 *@param length 数据长度
 */
static void internal_data_send(Sdk_SessionContext *sess, uint8_t *payload, uint16_t length)
{
    Sdk_SendParam param;
    param.channel = sess->channel;
    param.dataBuffer = payload;
    param.dataSize = length;
    if (g_sdk_cbs.send_cb != NULL)
    {
        g_sdk_cbs.send_cb(&param);
    }
    else
    {
        Sdk_LogError("Sdk_Send is not register!");
    }
}

/*
 *@brief 选择SE Applet Aid
 *@param sess 会话上下文
 */
static void ICCE_SE_SelectAppletAid(Sdk_SessionContext *sess)
{
    const uint8_t se_applet_aid[] = {0x49, 0x43, 0x43, 0x45, 0x44, 0x4b, 0x56, 0x76, 0x31};
    uint8_t se_apdu[32];
    memcpy(se_apdu, "\x00\xA4\x04\x00", 4);
    se_apdu[4] = sizeof(se_applet_aid);
    memcpy(se_apdu + 5, se_applet_aid, sizeof(se_applet_aid));
    // Sdk_LogDebugHex("[DEBUG] SE APDU TX", se_apdu, 5 + sizeof(se_applet_aid));
    internal_apdu_enqueue(sess, se_apdu, 5 + sizeof(se_applet_aid));
}

/*
 *@brief 选择DK Applet Aid
 *@param sess 会话上下文
 */
static void ICCE_DK_SelectAppletAid(Sdk_SessionContext *sess)
{
    const uint8_t dk_applet_aid[] = {0xA0, 0x00, 0x00, 0x08, 0x68, 0x49, 0x43, 0x43, 0x45, 0x44, 0x4B, 0x76, 0x31};
    uint8_t dk_apdu[32];
    memcpy(dk_apdu, "\x00\xA4\x04\x00", 4);
    dk_apdu[4] = sizeof(dk_applet_aid);
    memcpy(dk_apdu + 5, dk_applet_aid, sizeof(dk_applet_aid));
    internal_data_send(sess, dk_apdu, 5 + sizeof(dk_applet_aid));
}

/*
 *@brief 获取GPD数据
 *@param sess 会话上下文
 */
static void ICCE_DK_GPD(Sdk_SessionContext *sess)
{
    uint8_t dk_apdu[MAX_APDU_COMMAND_LENGTH];
    uint16_t dk_apdu_length = 0;
    uint8_t tag[2]; // 栈变量，替代全局 tag，防止多连接认证竞争
    if (sess->channel.channel_type != SDK_CHANNEL_TYPE_NFC)
    {
        tag[0] = 0x9F;
        tag[1] = 0x1E;
        dk_apdu_length = apducommand_addtag(tag, sizeof(g_reader_id), g_reader_id, dk_apdu, dk_apdu_length);
        tag[1] = 0x37;
        dk_apdu_length = apducommand_addtag(tag, sizeof(sess->reader_rnd), sess->reader_rnd, dk_apdu, dk_apdu_length);
        tag[1] = 0x0C;
        dk_apdu_length = apducommand_addtag(tag, sizeof(sess->reader_key_parameter), sess->reader_key_parameter,
                                            dk_apdu, dk_apdu_length);
    }
    else
    {
        dk_apdu_length = 5;
        memcpy(dk_apdu, "\x80\xCA\x00\x00\x00", dk_apdu_length);

        tag[0] = 0x9F;
        tag[1] = 0x1E;
        dk_apdu_length = apducommand_addtag(tag, sizeof(g_reader_id), g_reader_id, dk_apdu, dk_apdu_length);
        tag[1] = 0x37;
        dk_apdu_length = apducommand_addtag(tag, sizeof(sess->reader_rnd), sess->reader_rnd, dk_apdu, dk_apdu_length);
        tag[1] = 0x0C;
        dk_apdu_length = apducommand_addtag(tag, sizeof(sess->reader_key_parameter), sess->reader_key_parameter,
                                            dk_apdu, dk_apdu_length);

        dk_apdu[4] = dk_apdu_length - 5;
    }

    if (sess->channel.channel_type != SDK_CHANNEL_TYPE_NFC)
    {
        uint16_t input_length = buildTLV(0x01, dk_apdu, dk_apdu_length, sess->tlv_buffer);
        sess->parsed_buffer_length = build_frame_from_se_apdu(sess->parsed_buffer, MESSAGE_TYPE_AUTH, COMMAND_TYPE_AUTH,
                                                              0x10, sess->tlv_buffer, input_length);
        internal_data_send(sess, sess->parsed_buffer, sess->parsed_buffer_length);
    }
    else
    {
        // Sdk_LogDebugHex("[DEBUG] DK APDU TX", dk_apdu, dk_apdu_length);
        internal_data_send(sess, dk_apdu, dk_apdu_length);
    }
}

/*
 *@brief 内部认证
 *@param sess 会话上下文
 *@param p1 P1参数
 */
static void ICCE_Internal_Auth(Sdk_SessionContext *sess, uint8_t p1)
{
    uint8_t se_apdu[MAX_APDU_COMMAND_LENGTH];
    Sdk_ApduParam param;
    uint16_t se_apdu_length;
    memcpy(se_apdu, "\x00\x80\x00\x00\x00", 5);
    if (p1 == 0x80)
    {
        se_apdu[2] = p1;
    }
    memcpy(se_apdu + 5, sess->dk_apdu_resp, sess->dk_apdu_resp_length);
    se_apdu_length = 5 + sess->dk_apdu_resp_length;
    se_apdu[4] = se_apdu_length - 5;
    // Sdk_LogDebugHex("[DEBUG] SE APDU TX", se_apdu, se_apdu_length);

    internal_apdu_enqueue(sess, se_apdu, se_apdu_length);
}

/*
 *@brief DK认证
 *@param sess 会话上下文
 */
static void ICCE_DK_AUTH(Sdk_SessionContext *sess)
{
    uint8_t dk_apdu[MAX_APDU_COMMAND_LENGTH];
    uint16_t dk_apdu_length;
    if (sess->channel.channel_type != SDK_CHANNEL_TYPE_NFC)
    {

        memcpy(dk_apdu, sess->apdu_resp, sess->apdu_resp_length);
        dk_apdu_length = sess->apdu_resp_length;
    }
    else
    {

        memcpy(dk_apdu, "\x80\x80\x00\x00\x00", 5);
        memcpy(dk_apdu + 5, sess->apdu_resp, sess->apdu_resp_length);
        dk_apdu_length = 5 + sess->apdu_resp_length;
        dk_apdu[4] = dk_apdu_length - 5;
    }

    if (sess->channel.channel_type != SDK_CHANNEL_TYPE_NFC)
    {
        uint16_t input_length = buildTLV(0x02, dk_apdu, dk_apdu_length, sess->tlv_buffer);
        sess->parsed_buffer_length = build_frame_from_se_apdu(sess->parsed_buffer, MESSAGE_TYPE_AUTH, COMMAND_TYPE_AUTH,
                                                              0x10, sess->tlv_buffer, input_length);
        internal_data_send(sess, sess->parsed_buffer, sess->parsed_buffer_length);
    }
    else
    {
        // Sdk_LogDebugHex("[DEBUG] DK APDU TX", dk_apdu, dk_apdu_length);
        internal_data_send(sess, dk_apdu, dk_apdu_length);
    }
}

/*
 *@brief 外部认证
 *@param sess 会话上下文
 *@param p1 通道信息
 */
static void ICCE_External_Auth(Sdk_SessionContext *sess, uint8_t p1)
{
    uint8_t se_apdu[MAX_APDU_COMMAND_LENGTH];
    uint16_t se_apdu_length;
    memcpy(se_apdu, "\x80\x86\x00\x00\x00", 5);
    memcpy(se_apdu + 5, sess->dk_apdu_resp, sess->dk_apdu_resp_length);
    se_apdu_length = 5 + sess->dk_apdu_resp_length;
    se_apdu[2] = p1;
    se_apdu[4] = se_apdu_length - 5;
    // Sdk_LogDebugHex("[DEBUG] SE APDU TX", se_apdu, se_apdu_length);

    internal_apdu_enqueue(sess, se_apdu, se_apdu_length);
}
/*
 *@brief 离线认证
 *@param sess 会话上下文
 */
static void ICCE_Sdk_Offline_Auth(Sdk_SessionContext *sess)
{
    uint8_t se_apdu[MAX_APDU_COMMAND_LENGTH];
    uint16_t se_apdu_length;
    memcpy(se_apdu, "\x80\x58\x00\x00\x00", 5);
    memcpy(se_apdu + 5, sess->dk_apdu_resp, sess->dk_apdu_resp_length);
    se_apdu_length = 5 + sess->dk_apdu_resp_length;
    se_apdu[4] = se_apdu_length - 5;
    // Sdk_LogDebugHex("[DEBUG] SE APDU TX", se_apdu, se_apdu_length);
    internal_apdu_enqueue(sess, se_apdu, se_apdu_length);
}
/*
 *@brief 读二进制文件(profiles)
 *@param sess 会话上下文
 */
static void ICCE_Sdk_ReadBinary(Sdk_SessionContext *sess)
{
    uint8_t dk_apdu[MAX_APDU_COMMAND_LENGTH];
    uint16_t dk_apdu_length;
    memcpy(dk_apdu, "\x00\xB0\x83\x00\x00", 5);
    dk_apdu_length = 5;
    // Sdk_LogDebugHex("[DEBUG] SE APDU TX", se_apdu, se_apdu_length);
    internal_data_send(sess, dk_apdu, dk_apdu_length);
}

/*
 *@brief 加密解密
 *@param sess 会话上下文
 *@param type 类型
 *@param channel_num 通道号
 *@param dataBuffer 数据缓冲区
 *@param dataSize 数据大小
 */
static void Sdk_Encrypt_Decrypt(Sdk_SessionContext *sess, uint8_t type, uint8_t channel_num, uint8_t *dataBuffer,
                                uint16_t dataSize)
{
    uint8_t se_apdu[MAX_APDU_COMMAND_LENGTH];
    uint8_t IV[16] = {0};
    uint8_t len = 9;
    memcpy(se_apdu, "\x84\xFA\x00\x00\x00\x00\x00\x00\x00", 9);
    se_apdu[8] = channel_num;
    memcpy(se_apdu + 9, IV, 16);
    len += 16;
    memcpy(se_apdu + len, dataBuffer, dataSize);
    len += dataSize;
    se_apdu[3] = type;
    se_apdu[4] = len - 5;

    // Sdk_LogDebugHex("[DEBUG] SE APDU TX", se_apdu, len);
    internal_apdu_enqueue(sess, se_apdu, len);
}

/*
 *@brief 处理延迟的RKE请求（当DataReport/认证占用通道时，RKE数据暂存，
 *        等当前流程结束后再自动执行，保证两个指令都能正常完成）
 *@param sess 会话上下文（刚完成上一流程的会话，将被复用为RKE会话）
 */
static void Sdk_ProcessPendingRKE(Sdk_SessionContext *sess)
{
    if (!sess || !sess->has_pending_rke)
    {
        return;
    }
    if (sess->pending_rke_length == 0 || sess->pending_rke_length > MAX_APDU_COMMAND_LENGTH)
    {
        sess->has_pending_rke = 0;
        return;
    }

    Sdk_LogError("Processing pending RKE on channel %d, len=%d", sess->channel.id[0], sess->pending_rke_length);

    // 将会话重新初始化为RKE处理模式
    sess->is_active = 1;
    sess->status = STATE_MACHINE_RKE_REQUEST;
    sess->overall_ticks = 0;

    // 将暂存的RKE密文数据复制到dk_apdu_resp，供SE解密使用
    memcpy(sess->dk_apdu_resp, sess->pending_rke_data, sess->pending_rke_length);
    sess->dk_apdu_resp_length = sess->pending_rke_length;

    // 清除pending标记
    sess->pending_rke_length = 0;
    sess->has_pending_rke = 0;

    // 发起SE解密（与Sdk_SendCallback中RKE_REQUEST分支逻辑一致）
    Sdk_Encrypt_Decrypt(sess, 0x81, sess->channel.id[0] + 1, sess->dk_apdu_resp, sess->dk_apdu_resp_length);
}

// ========================================================================
// SDK 任务调度
// ========================================================================

/**
 *@brief 注册APDU回调
 *@param apduFunc 回调函数
 */
void Sdk_RegisterApdu(Sdk_Apdu apduFunc)
{
    if (apduFunc)
    {
        g_sdk_cbs.apdu_cb = apduFunc;
    }
}
/**
 *@brief 注册发送回调
 *@param sendFunc 回调函数
 */
void Sdk_RegisterSend(Sdk_Send sendFunc)
{
    if (sendFunc)
    {
        g_sdk_cbs.send_cb = sendFunc;
    }
}
/**
 *@brief 注册发送回调
 *@param sendFunc 回调函数
 */
void Sdk_RegisterRKE(Sdk_RKE rkeFunc)
{
    if (rkeFunc)
    {
        g_sdk_cbs.rke_cb = rkeFunc;
    }
}
/**
 *@brief 注册认证结果通知回调
 *@param notifyAuthResultFunc 回调函数
 */
void Sdk_RegisterNotifyAuthResult(Sdk_NotifyAuthResult notifyAuthResultFunc)
{

    if (notifyAuthResultFunc != NULL)
    {
        g_sdk_cbs.notify_auth_cb = notifyAuthResultFunc;
    }
}
/**
 *@brief 注册日志获取回调
 *@param getLogFunc 回调函数
 */
void Sdk_RegisterGetLogCallback(Sdk_GetLogCallback getLogFunc)
{
    if (getLogFunc != NULL)
    {
        g_sdk_cbs.get_log_cb = getLogFunc;
    }
}
/**
 *@brief 注册版本获取回调
 *@param getVersionFunc 回调函数
 */
void Sdk_RegisterGetVersionCallback(Sdk_GetVersionCallback getVersionFunc)
{
    if (getVersionFunc != NULL)
    {
        g_sdk_cbs.get_version_cb = getVersionFunc;
    }
}
/**
 *@brief 注册标定数据通知回调
 *@param notifyCalibFunc 回调函数
 */
void Sdk_RegisterNotifyCalibData(Sdk_NotifyCalibData notifyCalibFunc)
{
    if (notifyCalibFunc != NULL)
    {

        g_sdk_cbs.notify_calib_cb = notifyCalibFunc;
    }
}
/**
 *@brief 注册数据上报回调
 *@param dataReportFunc 回调函数
 */
void Sdk_RegisterDataReportCallback(Sdk_DataReportCallback dataReportFunc)
{
    if (dataReportFunc != NULL)
    {
        g_sdk_cbs.data_report_cb = dataReportFunc;
    }
}

/*
 *@brief 初始化SDK
 */
void Sdk_Init(void)
{

    memset(g_Sessions, 0, sizeof(g_Sessions));
    memset(&g_HwMgr, 0, sizeof(g_HwMgr));

    g_Sessions[0].is_active = 1;
    g_Sessions[0].status = STATE_MACHINE_INIT;
    g_Sessions[0].channel.channel_type = SDK_CHANNEL_TYPE_NFC;
    g_Sessions[0].channel.id[0] = 0x00;
    ICCE_SE_SelectAppletAid(&g_Sessions[0]);
    Sdk_LogError("SDK INIT");
    Trigger_Hw_Send();
}

/*
 *@brief 触发SE发送
 */
static void Trigger_Hw_Send(void)
{
    Sdk_LogError("count%d,is busy%d ", g_HwMgr.count, g_HwMgr.is_hw_busy);
    if ((g_HwMgr.count > 0) && (!g_HwMgr.is_hw_busy))
    { // TODO
        Sdk_ApduParam param;
        g_HwMgr.is_hw_busy = 1;
        g_HwMgr.hw_step_ticks = 0;
        param.dataBuffer = g_HwMgr.queue[g_HwMgr.head].apdu;
        param.dataSize = g_HwMgr.queue[g_HwMgr.head].apdu_len;
        if (g_sdk_cbs.apdu_cb != NULL)
        {
            g_sdk_cbs.apdu_cb(&param);
        }
        else
        {
            Sdk_LogError("Sdk_Apdu is not register!");
        }
    }
}

/*
 *@brief 结束会话（并自动处理该通道上排队等待的RKE请求）
 *@param sess 会话上下文
 */
static void Sdk_FinishSession(Sdk_SessionContext *sess)
{
    Sdk_LogError("end session:%d,%d", sess->status, sess->is_active);
    if (sess)
    {
        // 先检查pending RKE，再设置 is_active=0：
        // 如果先置0再检查，中间窗口期内通道看似空闲，
        // DataReport 定时器可能抢走会话，导致 pending RKE 丢失
        if (sess->has_pending_rke)
        {
            // ProcessPendingRKE 内部会重新设置 is_active=1
            Sdk_ProcessPendingRKE(sess);
        }
        else
        {
            sess->is_active = 0;
        }
    }
}

/*
 *@brief 通知蓝牙认证结果
 *@param result 结果
 *@param sess 会话上下文
 */
static void Sdk_NotifyBLEResult(Sdk_SessionContext *sess, uint8_t type, uint8_t len, uint8_t value)
{
    // 使用会话独立的缓冲区
    sess->tlv_buffer[0] = type;
    sess->tlv_buffer[1] = len;
    sess->tlv_buffer[2] = value;
    sess->parsed_buffer_length = build_frame_from_se_apdu(sess->parsed_buffer, MESSAGE_TYPE_NOTIFICATION,
                                                          COMMAND_TYPE_CMD, 0x10, sess->tlv_buffer, 0x03);
    internal_data_send(sess, sess->parsed_buffer, sess->parsed_buffer_length);
}

/*
 *@brief 获取会话索引
 *@param nChannel 通道信息
 *@return 会话索引
 */
static int Sdk_GetSessionIndex(Sdk_Channel *nChannel)
{
    if (nChannel == NULL)
    {
        return -1;
    }
    if (nChannel->channel_type == SDK_CHANNEL_TYPE_NFC)
    {
        if (nChannel->id[0] < 2)
        {
            return nChannel->id[0];
        }
    }
    else if (nChannel->channel_type == SDK_CHANNEL_TYPE_BLE)
    {
        if (nChannel->id[0] < 4)
        {
            return 2 + nChannel->id[0];
        }
    }
    return -1;
}

/*
 *@brief 分配会话
 *@param nChannel 通道信息
 *@return 会话上下文
 */
static Sdk_SessionContext *Sdk_AllocateSession(Sdk_Channel *nChannel)
{
    Sdk_SessionContext *sess = NULL;
    int index;
    if (nChannel == NULL)
    {
        return NULL;
    }

    index = Sdk_GetSessionIndex(nChannel);
    if (index >= 0 && index < SDK_MAX_SESSIONS)
    {
        sess = &g_Sessions[index];
        memset(sess, 0, sizeof(Sdk_SessionContext));
        sess->overall_ticks = 0;
        Sdk_LogError("currunt session:%d ", index);
        return sess;
    }

    return NULL;
}

/*
 *@brief 判断通道会话是否正忙于不兼容的操作（防止DataReport/RKE/Auth互相覆盖状态机）
 *@param nChannel 通道信息
 *@param allowed_status1 允许的状态1（调用者自身的状态之一）
 *@param allowed_status2 允许的状态2（可为0xFF表示不使用）
 *@return 1: 忙碌（不可覆盖）, 0: 空闲或状态兼容（可覆盖）
 */
static uint8_t Sdk_IsChannelBusyFor(Sdk_Channel *nChannel, uint8_t allowed_status1, uint8_t allowed_status2)
{
    int index;
    if (nChannel == NULL)
    {
        return 0;
    }
    index = Sdk_GetSessionIndex(nChannel);
    if (index < 0 || index >= SDK_MAX_SESSIONS)
    {
        return 0;
    }

    if (!g_Sessions[index].is_active)
    {
        return 0; // 空闲，不忙
    }

    // 如果当前状态是调用者允许的兼容状态，可以覆盖
    if (g_Sessions[index].status == allowed_status1)
    {
        return 0;
    }
    if (allowed_status2 != 0xFF && g_Sessions[index].status == allowed_status2)
    {
        return 0;
    }

    // 通道正忙于其他操作，不可覆盖
    Sdk_LogError("Channel %d busy: cur_status=0x%02X, caller allows 0x%02X/0x%02X", nChannel->id[0],
                 g_Sessions[index].status, allowed_status1, allowed_status2);
    return 1;
}

/*
 *@brief 认证
 *@param nChannel 通道信息
 *@return 状态码
 */
uint8_t Sdk_Auth(Sdk_Channel *nChannel)
{

    Sdk_SessionContext *sess;
    Sdk_LogError("auth start!");
    if (nChannel == NULL || nChannel->idSize > SDK_CHANNEL_ID_MAX_SIZE)
    {
        return STATUS_FAILED;
    }

    // 防止覆盖正在进行的 RKE/DataReport/GetCalibData 会话：
    // 只允许覆盖空闲会话或同为认证流程的会话
    if (Sdk_IsChannelBusyFor(nChannel, STATE_MACHINE_SELECT_SE, STATE_MACHINE_INIT))
    {
        Sdk_LogError("Auth blocked: channel busy with non-auth operation");
        return STATUS_FAILED;
    }

    sess = Sdk_AllocateSession(nChannel);
    if (!sess)
    {
        return STATUS_FAILED;
    }

    Sdk_LogError("cur session tick:%d,session status:%d", sess->overall_ticks, sess->status);
    // Sdk_AllocateSession 已清零会话，status==0 表示全新分配
    if (sess->status == 0)
    {
        sess->is_active = 1;
        sess->channel = *nChannel;
        sess->status = STATE_MACHINE_SELECT_SE;
        ICCE_SE_SelectAppletAid(sess);
        Trigger_Hw_Send();
    }
    return STATUS_SUCCESS;
}

/*
 *@brief 取消认证
 *@param nChannel 通道信息
 *@return 状态码
 */
uint8_t Sdk_CancelAuth(Sdk_Channel *nChannel)
{
    int i;
    if (nChannel == NULL)
    {
        Sdk_Init();
        return STATUS_SUCCESS;
    }
    for (i = 0; i < SDK_MAX_SESSIONS; i++)
    {
        if (g_Sessions[i].is_active && Sdk_IsChannelEqual(&g_Sessions[i].channel, nChannel))
        {
            // 通过 Sdk_FinishSession 关闭，确保 pending RKE 不被丢失
            Sdk_FinishSession(&g_Sessions[i]);
        }
    }
    return STATUS_SUCCESS;
}

/*
 *@brief 释放会话
 *@param nChannel 通道信息
 */
void Sdk_Release(Sdk_Channel *nChannel)
{
    Sdk_CancelAuth(nChannel);
}

/*
 *@brief 数据上报
 *@param param 数据上报参数
 *@return 状态码
 */
uint8_t Sdk_DataReport(Sdk_DataReportParam *param)
{
    Sdk_SessionContext *sess;
    Sdk_LogError("data report start");
    uint16_t tlv_len;
    uint16_t actual_size;
    if (param == NULL)
    {
        return STATUS_FAILED;
    }

    // 防止覆盖正在进行的 RKE/Auth/GetCalibData 会话：
    // 只允许覆盖空闲会话或同为 DataReport 的会话（上一轮遗留）
    if (Sdk_IsChannelBusyFor(&param->channel, STATE_MACHINE_DATAREPORT_REQUEST, STATE_MACHINE_DATAREPORT_RESPONSE))
    {
        Sdk_LogError("DataReport blocked: channel busy with RKE/Auth");
        return STATUS_FAILED; // MCU 应在 2s 后重试，届时 RKE 应已完成
    }

    sess = Sdk_AllocateSession(&param->channel);
    if (!sess)
    {
        return STATUS_FAILED;
    }

    memset(sess, 0, sizeof(Sdk_SessionContext));
    sess->is_active = 1;
    sess->channel = param->channel;
    sess->status = STATE_MACHINE_DATAREPORT_REQUEST;

    actual_size = 0;
    if (param->dataSize > 0 && param->dataSize <= MAX_APDU_COMMAND_LENGTH)
    {
        actual_size = param->dataSize;
    }

    tlv_len = buildTLV(0x03, param->dataBuffer, actual_size, sess->tlv_buffer);
    tlv_len = padding_data(sess->tlv_buffer, tlv_len);
    Sdk_Encrypt_Decrypt(sess, 0x01, sess->channel.id[0] + 1, sess->tlv_buffer, tlv_len);
    Trigger_Hw_Send();
    return STATUS_SUCCESS;
}

/*
 *@brief 获取校准数据
 *@param req 校准数据请求
 *@return 状态码
 */
uint8_t Sdk_GetCalibData(Sdk_GetCalibReq *req)
{
    Sdk_SessionContext *sess;
    uint16_t calib_len;
    if (req == NULL)
    {
        return STATUS_FAILED;
    }

    // 防止覆盖正在进行的 RKE/Auth/DataReport 会话
    if (Sdk_IsChannelBusyFor(&req->channel, STATE_MACHINE_CALIBDATA, 0xFF))
    {
        Sdk_LogError("GetCalibData blocked: channel busy");
        return STATUS_FAILED;
    }

    sess = Sdk_AllocateSession(&req->channel);
    if (!sess)
    {
        return STATUS_FAILED;
    }

    memset(sess, 0, sizeof(Sdk_SessionContext));
    sess->is_active = 1;
    sess->channel = req->channel;
    sess->status = STATE_MACHINE_CALIBDATA;

    calib_len = buildTLV(0x06, req->dataBuffer, req->dataSize, sess->tlv_buffer);
    Sdk_Encrypt_Decrypt(sess, 0x01, sess->channel.id[0] + 1, sess->tlv_buffer, calib_len);
    Trigger_Hw_Send();
    return STATUS_SUCCESS;
}

/*
 *@brief RKE回调
 *@param nChannel 通道信息
 *@param param RKE回调参数
 *@return 状态码
 */
uint8_t Sdk_RKECallback(Sdk_Channel *nChannel, Sdk_RKECallbackParam *param)
{
    Sdk_SessionContext *sess;
    int i;
    int padding_len;
    int sess_found = 0;
    if (nChannel == NULL || param == NULL)
    {
        return STATUS_FAILED;
    }

    // 优先匹配RKE_REQUEST状态的会话，避免与DataReport会话混淆
    for (i = 0; i < SDK_MAX_SESSIONS; i++)
    {
        if (g_Sessions[i].is_active && g_Sessions[i].status == STATE_MACHINE_RKE_REQUEST &&
            Sdk_IsChannelEqual(&g_Sessions[i].channel, nChannel))
        {
            sess = &g_Sessions[i];
            sess_found = 1;
            Sdk_LogError("RKE active channel:%d, state:%d", sess->channel.id[0], sess->is_active);
            break;
        }
    }
    if (!sess_found)
    {
        Sdk_LogError("RKE no matching session found!");
        return STATUS_FAILED;
    }

    sess->status = STATE_MACHINE_RKE_RESPONSE;
    sess->tlv_buffer[0] = 0x00;
    sess->tlv_buffer[1] = 0x01;
    sess->tlv_buffer[2] = 0x00;
    sess->tlv_buffer[3] = param->rkeResult[0];
    sess->tlv_buffer[4] = 0x01;
    sess->tlv_buffer[5] = param->rkeResult[1];

    padding_len = padding_data(sess->tlv_buffer, 0x06);
    Sdk_LogDebugHex("cipher:", sess->tlv_buffer, padding_len);
    Sdk_Encrypt_Decrypt(sess, 0x01, sess->channel.id[0] + 1, sess->tlv_buffer, padding_len);
    Trigger_Hw_Send();
    return STATUS_SUCCESS;
}

/*
 *@brief 执行TSM命令
 *@param param TSM命令参数
 *@return 状态码
 */
uint8_t SEAgent_ExecTsmCmd(SEAgent_ExecTsmParam *param)
{
    (void)param;
    return STATUS_SUCCESS;
}

/*
 *@brief SEAgent APDU回调
 *@param result 结果
 *@param respData 响应数据
 *@param dataSize 响应数据大小
 *@return 状态码
 */
uint8_t SEAgent_ApduCallback(uint8_t result, uint8_t *respData, uint16_t dataSize)
{
    (void)result;
    (void)respData;
    (void)dataSize;
    return STATUS_SUCCESS;
}

/*
 *@brief SDK超时检查
 */
void Sdk_routine(void)
{
    // 检查所有会话存管时长以及app阻塞
    int i;
    Sdk_NotifyAuthResultParam localNotify; // 栈变量，防止重入覆盖

    // 处理ISR缓存的APDU响应（ISR只拷贝数据，主循环做队列操作+业务分发）
    if (g_HwMgr.apdu_response_ready)
    {
        Sdk_ProcessApduResponse();
    }

    // 检查SE指令队列堵塞
    if (g_HwMgr.count > 0)
    {
        Sdk_LogError("currrent queue count:%d,current state:%d", g_HwMgr.count, g_HwMgr.is_hw_busy);
        if (g_HwMgr.is_hw_busy)
        {
            g_HwMgr.hw_step_ticks++;
            if (g_HwMgr.hw_step_ticks > SE_CMD_TIMEOUT_TICKS)
            {
                // 超时出队也需要队列锁保护，与入队/正常出队保持一致
                if (Sdk_HwQueue_TryLock())
                {
                    Sdk_SessionContext *sess = g_HwMgr.queue[g_HwMgr.head].owner;
                    g_HwMgr.is_hw_busy = 0;
                    g_HwMgr.head = (g_HwMgr.head + 1) % SDK_MAX_HW_QUEUE;
                    g_HwMgr.count--;
                    Sdk_HwQueue_Unlock();

                    if (sess)
                    {
                        localNotify.channel = sess->channel;
                        localNotify.errorCode = ERR_COMMAND_TIMEOUT;
                        localNotify.sw[0] = 0;
                        localNotify.sw[1] = 0;
                        Sdk_ReportResult(&localNotify);
                        Sdk_FinishSession(sess);
                    }
                }
                // Sdk_LogError("");
                // Trigger_Hw_Send();
            }
        }
        else
        {
            Sdk_LogError("send queue!");
            Trigger_Hw_Send();
        }
    }
    for (i = 0; i < SDK_MAX_SESSIONS; i++)
    {
        if (g_Sessions[i].is_active)
        {
            g_Sessions[i].overall_ticks++;
            if (g_Sessions[i].overall_ticks > SESSION_TIMEOUT_TICKS)
            {
                localNotify.channel = g_Sessions[i].channel;
                localNotify.errorCode = ERR_AUTH_TIMEOUT;
                localNotify.sw[0] = 0;
                localNotify.sw[1] = 0;
                Sdk_ReportResult(&localNotify);
                // if (g_Sessions[i].channel.channel_type == SDK_CHANNEL_TYPE_BLE) {
                //     Sdk_NotifyBLEResult(0x04, &g_Sessions[i]);
                // }
                Sdk_FinishSession(&g_Sessions[i]);
            }
            if (g_Sessions[i].is_waiting_peer)
            {
                g_Sessions[i].peer_step_ticks++;
                if (g_Sessions[i].peer_step_ticks > PEER_STEP_TIMEOUT_TICKS)
                {
                    localNotify.channel = g_Sessions[i].channel;
                    localNotify.errorCode = ERR_COMMAND_TIMEOUT;
                    localNotify.sw[0] = 0;
                    localNotify.sw[1] = 0;
                    Sdk_ReportResult(&localNotify);
                    Sdk_FinishSession(&g_Sessions[i]);
                }
            }
        }
    }
}

// ========================================================================
// 回调函数 认证流程控制
// ========================================================================

/*
 *@brief APDU回调（ISR安全：仅拷贝响应数据到缓冲区，不操作队列）
 *        实际出队+业务处理由主循环 Sdk_ProcessApduResponse 完成
 */
uint8_t Sdk_ApduCallback(uint8_t result, uint8_t *respData, uint16_t dataSize)
{
    g_HwMgr.is_hw_busy = 0;

    if (g_HwMgr.apdu_response_ready)
    {
        Sdk_LogError("apdu response overflow!");
        return STATUS_FAILED;
    }

    g_HwMgr.apdu_response_result = result;
    if (respData != NULL && dataSize > 0)
    {
        uint16_t copySize = (dataSize <= MAX_APDU_COMMAND_LENGTH) ? dataSize : MAX_APDU_COMMAND_LENGTH;
        memcpy(g_HwMgr.apdu_response_data, respData, copySize);
        g_HwMgr.apdu_response_size = copySize;
    }
    else
    {
        g_HwMgr.apdu_response_size = 0;
    }
    g_HwMgr.apdu_response_ready = 1;
    return STATUS_SUCCESS;
}

/*
 *@brief 处理ISR缓存的APDU响应（主循环调用）
 */
static void Sdk_ProcessApduResponse(void)
{
    Sdk_SessionContext *sess;
    uint8_t result;
    uint8_t *respData;
    uint16_t dataSize;
    uint8_t expected_status;
    uint8_t retry = 0;
    uint8_t tag[2];

    if (!g_HwMgr.apdu_response_ready)
    {
        return;
    }

    result = g_HwMgr.apdu_response_result;
    dataSize = g_HwMgr.apdu_response_size;
    respData = (dataSize > 0) ? g_HwMgr.apdu_response_data : NULL;
    g_HwMgr.apdu_response_ready = 0;

    // --- 以下为原 Sdk_ApduCallback 的队列操作和业务逻辑 ---

    while (!Sdk_HwQueue_TryLock())
    {
        retry++;
        if (retry > 100)
        {
            Sdk_LogError("dequeue lock timeout!");
            return;
        }
    }

    if (g_HwMgr.count == 0)
    {
        Sdk_HwQueue_Unlock();
        return;
    }
    sess = g_HwMgr.queue[g_HwMgr.head].owner;
    expected_status = g_HwMgr.queue[g_HwMgr.head].expected_status;
    g_HwMgr.head = (g_HwMgr.head + 1) % SDK_MAX_HW_QUEUE;
    g_HwMgr.count--;

    Sdk_HwQueue_Unlock();

    if (!sess || !sess->is_active || sess->status != expected_status)
    {
        Sdk_LogError("Stale SE response discarded");
        Trigger_Hw_Send();
        return;
    }

    memset(sess->apdu_resp, 0, sizeof(sess->apdu_resp));
    if (respData != NULL && dataSize > 0)
    {
        memcpy(sess->apdu_resp, respData, dataSize);
        sess->apdu_resp_length = dataSize;
        parsingSW(sess, sess->apdu_resp, &sess->apdu_resp_length);
    }

    if (sess->status_word != 0x9000 || result != 0x00)
    {
        if ((sess->status == STATE_MACHINE_EXTERNAL_AUTH) && (sess->status_word == 0x6401) && (result == 0x00))
        {
            sess->status = STATE_MACHINE_OFFLINE_AUTH;
            if (sess->channel.channel_type == SDK_CHANNEL_TYPE_BLE)
            {
                Sdk_NotifyBLEResult(sess, 0x02, 0x01, 0x06);
            }
            else if (sess->channel.channel_type == SDK_CHANNEL_TYPE_NFC)
            {
                ICCE_Sdk_ReadBinary(sess);
            }
        }
        else
        {
            NotifyAuthResultParam.channel = sess->channel;
            NotifyAuthResultParam.errorCode = ERR_AUTH_BASE + sess->status;
            Sdk_ReportResult(&NotifyAuthResultParam);

            if (sess->channel.channel_type == SDK_CHANNEL_TYPE_BLE)
            {
                if ((sess->status == 0x03) || (sess->status == 0x04))
                {
                    result = 0x02;
                }
                else if ((sess->status == 0x05) || (sess->status == 0x06))
                {
                    result = 0x01;
                }
            }
            Sdk_LogError("cur channel:%d,errorcode:%d,SW:%02x", NotifyAuthResultParam.channel.id[0], sess->status_word);
            Sdk_FinishSession(sess);
            Trigger_Hw_Send();
        }
        return;
    }

    Sdk_LogDebug("Current State: 0x%02X", sess->status);
    switch (sess->status)
    {
    case STATE_MACHINE_INIT:
        memcpy(tag, "\x9F\x08", 2);
        tlvdata_parsing(tag, sess->apdu_resp, sess->apdu_resp_length, applet_version, sizeof(applet_version));
        memcpy(versionParam.applet_version, applet_version, sizeof(versionParam.applet_version));
        memcpy(tag, "\x5A\x00", 2);
        tlvdata_parsing(tag, sess->apdu_resp, sess->apdu_resp_length, SEID, sizeof(SEID));
        memcpy(versionParam.SEID, SEID, sizeof(SEID));
        Sdk_GetVersionInfo();
        Sdk_FinishSession(sess);
        break;
    case STATE_MACHINE_SELECT_SE:
        memcpy(tag, "\x9F\x37", 2);
        tlvdata_parsing(tag, sess->apdu_resp, sess->apdu_resp_length, sess->reader_rnd, sizeof(sess->reader_rnd));
        memcpy(tag, "\x9F\x0C", 2);
        tlvdata_parsing(tag, sess->apdu_resp, sess->apdu_resp_length, sess->reader_key_parameter,
                        sizeof(sess->reader_key_parameter));
        sess->status = STATE_MACHINE_GPD;
        ICCE_DK_GPD(sess);
        break;
    case STATE_MACHINE_GPD:
        sess->status = STATE_MACHINE_INTERNAL_AUTH;
        break;
    case STATE_MACHINE_INTERNAL_AUTH:
        sess->status = STATE_MACHINE_AUTH;
        ICCE_DK_AUTH(sess);
        break;
    case STATE_MACHINE_EXTERNAL_AUTH:
        if (result == 0)
        {
            Sdk_LogError("Auth success! ");
        }
        memcpy(NotifyAuthResultParam.cardId, sess->card_id, 16);
        NotifyAuthResultParam.channel = sess->channel;
        NotifyAuthResultParam.errorCode = 0;
        Sdk_ReportResult(&NotifyAuthResultParam);
        if (sess->channel.channel_type == SDK_CHANNEL_TYPE_BLE)
        {
            Sdk_NotifyBLEResult(sess, 0x02, 0x01, 0x00);
        }
        Sdk_FinishSession(sess);
        break;
    case STATE_MACHINE_DATAREPORT_REQUEST:
        sess->parsed_buffer_length =
            build_frame_from_se_apdu(sess->parsed_buffer, MESSAGE_TYPE_NOTIFICATION, COMMAND_TYPE_CMD, 0x18,
                                     sess->apdu_resp, sess->apdu_resp_length);
        internal_data_send(sess, sess->parsed_buffer, sess->parsed_buffer_length);
        Sdk_FinishSession(sess);
        break;
    case STATE_MACHINE_DATAREPORT_RESPONSE:
        if (g_sdk_cbs.data_report_cb != NULL)
        {
            g_sdk_cbs.data_report_cb(&sess->channel, sess->apdu_resp, sess->apdu_resp_length);
        }
        Sdk_FinishSession(sess);
        break;
    case STATE_MACHINE_CALIBDATA:
        Sdk_FinishSession(sess);
        break;
    case STATE_MACHINE_RKE_REQUEST:
        rkeParam.channel = sess->channel;
        rkeParam.RKEcmd = sess->apdu_resp[2];
        if (g_sdk_cbs.rke_cb != NULL)
        {
            g_sdk_cbs.rke_cb(&rkeParam);
        }
        break;
    case STATE_MACHINE_RKE_RESPONSE:
        sess->parsed_buffer_length = build_frame_from_se_apdu(
            sess->parsed_buffer, MESSAGE_TYPE_COMMAND, COMMAND_TYPE_RKE, 0x08, sess->apdu_resp, sess->apdu_resp_length);
        internal_data_send(sess, sess->parsed_buffer, sess->parsed_buffer_length);
        Sdk_FinishSession(sess);
        break;
    }

    Trigger_Hw_Send();
}

/*
 *@brief DK回调
 *@param nChannel 通道信息
 *@param result 结果
 *@param respData 响应数据
 *@param dataSize 响应数据大小
 *@return 状态码
 */
uint8_t Sdk_SendCallback(Sdk_Channel *nChannel, uint8_t result, uint8_t *respData, uint16_t dataSize)
{
    Sdk_SessionContext *sess = NULL;
    Sdk_NotifyAuthResultParam localNotify; // 栈变量，防止重入覆盖
    ICCE_FrameInfo_t current_frame = {0};
    int i;
    uint8_t tempStatus = 0;
    Sdk_LogError("sendcallback success! response data size:%d ", dataSize);

    if (nChannel->channel_type == SDK_CHANNEL_TYPE_BLE)
    {

        if (dataSize > MAX_APDU_COMMAND_LENGTH)
        {
            dataSize = MAX_APDU_COMMAND_LENGTH;
        }
        if (nChannel->protocol_type == 0x01)
        {
            if (icce_parse_frame(nChannel, respData, dataSize, &current_frame) != 0)
            {
                return STATUS_FAILED;
            }
            if (current_frame.message_id == 0x02)
            {
                tempStatus = STATE_MACHINE_RKE_REQUEST;
            }
        }

        // 复制payload到临时缓冲区（后续会复制到匹配的session中）
        // 此处先暂存在current_frame的payload指针中
    }

    // 会话匹配（简化逻辑）：
    // 同通道上 Auth 先完成，DataReport/RKE 由 IsChannelBusyFor + pending 保证互斥，
    // 且 RKE 执行完即关闭会话——因此 SendCallback 收到 RKE 时通道上不可能有 RKE 会话。
    // 唯一特殊处理：RKE 到达时若通道有其他活跃会话 → 暂存到 pending 队列。

    if (tempStatus == STATE_MACHINE_RKE_REQUEST)
    {
        // RKE 到达，检查通道是否被其他操作占用
        for (i = 0; i < SDK_MAX_SESSIONS; i++)
        {
            if (g_Sessions[i].is_active && Sdk_IsChannelEqual(&g_Sessions[i].channel, nChannel))
            {
                // 通道被占用 → 挂起 RKE
                if (current_frame.payload_length > 0 && current_frame.payload_length <= MAX_APDU_COMMAND_LENGTH)
                {
                    memcpy(g_Sessions[i].pending_rke_data, current_frame.payload, current_frame.payload_length);
                    g_Sessions[i].pending_rke_length = current_frame.payload_length;
                    g_Sessions[i].has_pending_rke = 1;
                    Sdk_LogError("RKE queued on channel %d (pending), current status=0x%02X", nChannel->id[0],
                                 g_Sessions[i].status);
                }
                return STATUS_SUCCESS;
            }
        }
    }
    else
    {
        // 非RKE消息：找到本通道活跃会话
        for (i = 0; i < SDK_MAX_SESSIONS; i++)
        {
            if (g_Sessions[i].is_active && Sdk_IsChannelEqual(&g_Sessions[i].channel, nChannel))
            {
                sess = &g_Sessions[i];
                break;
            }
        }
    }

    if (!sess)
    {
        if (tempStatus == STATE_MACHINE_RKE_REQUEST)
        {
            sess = Sdk_AllocateSession(nChannel);
            if (sess)
            {
                sess->is_active = 1;
                sess->channel = *nChannel;
                sess->status = tempStatus;
            }
        }
    }

    if (!sess)
    {
        Sdk_LogError("sess null error \r\n");
        return STATUS_FAILED;
    }

    // 将响应数据复制到会话独立的缓冲区
    memset(sess->dk_apdu_resp, 0, MAX_APDU_COMMAND_LENGTH);
    if (nChannel->channel_type == SDK_CHANNEL_TYPE_BLE)
    {
        if (current_frame.payload_length <= MAX_APDU_COMMAND_LENGTH)
        {
            memcpy(sess->dk_apdu_resp, current_frame.payload, current_frame.payload_length);
            sess->dk_apdu_resp_length = current_frame.payload_length;
        }
    }
    else
    {
        if (dataSize <= MAX_APDU_COMMAND_LENGTH)
        {
            memcpy(sess->dk_apdu_resp, respData, dataSize);
            sess->dk_apdu_resp_length = dataSize;
            Sdk_LogError("copy response data size:%d", sess->dk_apdu_resp_length);
        }
    }

    Sdk_LogError("current channel%d,status:%d!", sess->channel.id[0], sess->status);
    sess->is_waiting_peer = 0;

    if (sess->channel.channel_type != SDK_CHANNEL_TYPE_BLE)
    {
        Sdk_LogError("parsing status word ");
        parsingSW(sess, sess->dk_apdu_resp, &sess->dk_apdu_resp_length);
        Sdk_LogError("parsed data size:%d", sess->dk_apdu_resp_length);
        if (sess->status_word != 0x9000)
        {
            localNotify.channel = *nChannel;
            localNotify.errorCode = ERR_AUTH_BASE + sess->status;

            Sdk_ReportResult(&localNotify);
            Sdk_FinishSession(sess);
            return STATUS_SUCCESS;
        }
    }

    Sdk_LogDebug("[DEBUG] Current State: 0x%02X !", sess->status);
    switch (sess->status)
    {

    case STATE_MACHINE_GPD:
    case STATE_MACHINE_INTERNAL_AUTH:
        Sdk_LogError("internal auth start!");
        if (nChannel->channel_type == SDK_CHANNEL_TYPE_NFC)
        {
            sess->status = STATE_MACHINE_INTERNAL_AUTH;
            Transform_GPD_Response(sess);
            ICCE_Internal_Auth(sess, 0x00);
        }
        else
        {
            if (current_frame.fsn == 0x00)
            {
                ICCE_Internal_Auth(sess, 0x80);
            }
            else
            {

                ICCE_Internal_Auth(sess, 0x00);
            }
        }
        break;

    case STATE_MACHINE_AUTH:
    {
        uint8_t p1;
        sess->status = STATE_MACHINE_EXTERNAL_AUTH;
        p1 = (sess->channel.channel_type == SDK_CHANNEL_TYPE_NFC) ? sess->channel.id[0] : sess->channel.id[0] + 1;
        ICCE_External_Auth(sess, p1);
    }
    break;

    case STATE_MACHINE_OFFLINE_AUTH:
        sess->status = STATE_MACHINE_EXTERNAL_AUTH;
        ICCE_Sdk_Offline_Auth(sess);
        break;

    case STATE_MACHINE_RKE_REQUEST:
        Sdk_Encrypt_Decrypt(sess, 0x81, nChannel->id[0] + 1, sess->dk_apdu_resp, sess->dk_apdu_resp_length);
        break;
    }

    Trigger_Hw_Send();
    return STATUS_SUCCESS;
}

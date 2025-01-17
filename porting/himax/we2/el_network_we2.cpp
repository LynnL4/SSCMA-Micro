/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Hongtai Liu (Seeed Technology Inc.)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "el_network_we2.h"

#include <hx_drv_scu.h>

#include "core/el_debug.h"

lwRingBuffer*         at_rbuf   = NULL;
static esp_at_t       at = {0};
static TaskHandle_t   at_rx_parser   = NULL;
static TaskHandle_t   status_handler = NULL;

static SemaphoreHandle_t at_got_response = NULL;
static SemaphoreHandle_t pubraw_complete = NULL;

static uint16_t send_len = 0;
static uint8_t fail_count = 0;

static resp_trigger_t resp_flow[] = {
    {AT_STR_RESP_OK,     resp_action_ok},
    {AT_STR_RESP_ERROR,  resp_action_error},
    {AT_STR_RESP_READY,  resp_action_ready},
    {AT_STR_RESP_WIFI_H, resp_action_wifi},
    {AT_STR_RESP_PUBRAW, resp_action_pubraw},
    {AT_STR_RESP_MQTT_H, resp_action_mqtt},
    {AT_STR_RESP_IP_H,   resp_action_ip},
    {AT_STR_RESP_NTP,    resp_action_ntp}
};

#ifdef CONFIG_EL_NETWORK_SPI_AT
static uint8_t send_seq = 0;
static uint8_t recv_seq = 0;
static TaskHandle_t spi_trans_ctrl = NULL;
volatile static SemaphoreHandle_t spi_sem = NULL;

static void spi_dma_cb(void* status);
static void at_port_handshake_cb(uint8_t group, uint8_t aIndex);

static void at_requset_write(esp_at_t* at) {
    static uint8_t value = 0;
    hx_drv_gpio_get_in_value(AON_GPIO0, &value);
    if (value == 1) {
        EL_LOGD("clean handshake before write");
        xTaskNotify(spi_trans_ctrl, AT_SPI_HANDSHAKE_FLAG, eNoAction);
        el_sleep(4);
    }

    static uint8_t send_req[7] = {0x01, 0x00, 0x00, 0xfe, send_seq, 0, 0};
    send_len = at->tbuf_len - at->sent_len > AT_DMA_TRAN_MAX_SIZE ? 
                AT_DMA_TRAN_MAX_SIZE : at->tbuf_len - at->sent_len;
    send_req[5] = (send_len) & 0xff;
    send_req[6] = (send_len >> 8) & 0xff;
    at->port->spi_write(send_req, 7);
}

#else
static uint8_t dma_rx[4] = {0};
static void at_port_txcb(void* arg);
static void at_port_rxcb(void* arg);
#endif

static void at_port_write(esp_at_t* at) {
    at->tbuf_len = strlen(at->tbuf);
#ifdef CONFIG_EL_NETWORK_SPI_AT
    at_requset_write(at);
#else
    at->port->uart_write(at->tbuf, strlen(at->tbuf));
#endif
}

// flush buffer by 254 bytes zero and CRLF util at-port can response
static void at_port_flush(esp_at_t* at) {
    EL_LOGW("FLUSH...\n");
    at->state = AT_STATE_PROCESS;
    uint32_t t = 0;
    memset((void*)at->tbuf, '0', 254);
    memcpy((void*)at->tbuf + 254, AT_STR_CRLF, strlen(AT_STR_CRLF));
    while (at->state == AT_STATE_PROCESS) {
        send_len = 256;
        at_port_write(at);
        if (t >= AT_SHORT_TIME_MS) {
            EL_LOGW("AT FLUSH TIMEOUT\n");
            return;
        }
        el_sleep(10);
        t += 10;
    }
}

static void at_port_init(esp_at_t* at) {
#ifdef CONFIG_EL_NETWORK_SPI_AT
    SCU_PAD_PULL_LIST_T pull_cfg;
    pull_cfg.pa0.pull_en = SCU_PAD_PULL_EN;
    pull_cfg.pa0.pull_sel = SCU_PAD_PULL_DOWN;
    pull_cfg.pb2.pull_en = SCU_PAD_PULL_EN;
    pull_cfg.pb2.pull_sel = SCU_PAD_PULL_DOWN;
    pull_cfg.pb3.pull_en = SCU_PAD_PULL_EN;
    pull_cfg.pb3.pull_sel = SCU_PAD_PULL_DOWN;
    hx_drv_scu_set_PA0_pinmux(SCU_PA0_PINMUX_AON_GPIO0_0, 0);
    hx_drv_scu_set_all_pull_cfg(&pull_cfg); 
    hx_drv_gpio_set_int_type(AON_GPIO0, GPIO_IRQ_TRIG_TYPE_EDGE_RISING);
    hx_drv_gpio_cb_register(AON_GPIO0, at_port_handshake_cb);
    hx_drv_gpio_set_input(AON_GPIO0);
    hx_drv_gpio_set_int_enable(AON_GPIO0, 1);

    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_M_DO_1, 0);
    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI_M_DI_1, 0);
    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI_M_SCLK_1, 1);
    hx_drv_scu_set_PB11_pinmux(SCU_PB11_PINMUX_SPI_M_CS, 1);

    at->port = hx_drv_spi_mst_get_dev(USE_DW_SPI_MST_S);
    if (at->port == NULL) {
        EL_ELOG("spi init error\n");
        return;
    }
    at->port->spi_open(DEV_MASTER_MODE, 4000000);
#else
    hx_drv_scu_set_PB6_pinmux(SCU_PB6_PINMUX_UART1_RX, 0);
    hx_drv_scu_set_PB7_pinmux(SCU_PB7_PINMUX_UART1_TX, 0);
    hx_drv_uart_init(USE_DW_UART_1, HX_UART1_BASE);
    at->port = hx_drv_uart_get_dev(USE_DW_UART_1);
    if (at->port == NULL) {
        EL_LOGD("uart init error\n");
        return;
    }
    at->port->uart_open(UART_BAUDRATE_921600);
    memset((void*)at->tbuf, 0, sizeof(at->tbuf));
    at->port->uart_read_udma(dma_rx, 1, (void*)at_port_rxcb);
#endif
}

static el_err_code_t at_send(esp_at_t* at, uint32_t timeout) {
    xSemaphoreTake(at_got_response, 0); // clear semaphore
    EL_LOGI("-> %s\n", at->tbuf);
    at->state = AT_STATE_PROCESS;
    at->sent_len = 0;
    at_port_write(at);

    if (xSemaphoreTake(at_got_response, timeout) == pdFALSE) {
        EL_LOGD("AT TIMEOUT\n");
        return EL_ETIMOUT;
    }

    if (at->state != AT_STATE_OK) {
        EL_LOGD("AT STATE ERROR: %d\n", at->state);
        return EL_EIO;
    }
    return EL_OK;
}

/************* AT command handler **************/

// update status for spi write, read spi when readable
static void spi_trans_task(void* arg) {
    static uint8_t value = 0;
    static uint32_t time = 0;
    static const uint8_t query[3] = {0x02, 0x04, 0x00};
    static const uint8_t read_hdr[3] = {0x04, 0x04, 0x00};
    static const uint8_t read_done[3] = {0x08, 0x04, 0x00};
    static const uint8_t write_hdr[3] = {0x03, 0x00, 0x00};
    static const uint8_t write_done[3] = {0x07, 0x00, 0x00};
    static uint8_t read_data[256] = {0};
    xSemaphoreTake(spi_sem, 0);
    while (1) {
        if (xTaskNotifyWait(ULONG_MAX, ULONG_MAX, NULL, portMAX_DELAY) != pdPASS) continue;

        SCB_CleanDCache_by_Addr((volatile void*)&(at.ctrl), 4);
        at.port->spi_write_then_read_dma(
            (void *)query, sizeof(query),
            (void *)&(at.ctrl), 4,
            (void *)spi_dma_cb
        );
        if (xSemaphoreTake(spi_sem, 20) != pdPASS) {
            EL_LOGW("spi dma query timeout\n");
            xSemaphoreGive(spi_sem);
            continue;
        }
        // EL_LOGI("[direct: %d, len: %d, seq: %d]", at.ctrl.direct, at.ctrl.len, at.ctrl.seq);

        if (at.ctrl.direct == 0x01) 
        {
            EL_LOGI("readable, len: %d, seq: %d", at.ctrl.len, at.ctrl.seq);
            recv_seq = (recv_seq + 1) % 256;

            SCB_CleanDCache_by_Addr((volatile void*)read_data, at.ctrl.len);
            at.port->spi_write_then_read_dma(
                (void *)read_hdr, sizeof(read_hdr),
                (void *)read_data, at.ctrl.len,
                (void *)spi_dma_cb
            );
            if (xSemaphoreTake(spi_sem, 20) != pdPASS) {
                EL_LOGW("spi dma read timeout\n");
                xSemaphoreGive(spi_sem);
                continue;
            }

            read_data[at.ctrl.len] = '\0';
            EL_LOGD("read: %s", (char*)read_data);
            for (uint16_t i = 0; i < at.ctrl.len; i++) {
                *at_rbuf << (char)read_data[i];
                if ((char)read_data[i] == '\n') {
                    xTaskNotifyGive(at_rx_parser);
                }
            }

            at.port->spi_write(read_done, sizeof(read_done));
            time = xTaskGetTickCount();
            do { 
                hx_drv_gpio_get_in_value(AON_GPIO0, &value);
                if (xTaskGetTickCount() > time + 10) break;
            } while (value == 1);
            EL_LOGD("read done\n");
            portYIELD();
        } 
        else if (at.ctrl.direct == 0x02) 
        {
            EL_LOGD("writeable, len: %d, seq: %d", at.ctrl.len, at.ctrl.seq);
            
            SCB_CleanDCache_by_Addr((volatile void*)at.tbuf, at.tbuf_len);
            at.port->spi_write_ptl(write_hdr, sizeof(write_hdr), 
                                   at.tbuf + at.sent_len, send_len, // at.ctrl.len < send_len ? at.ctrl.len : send_len, 
                                   (void *)spi_dma_cb);
            if (xSemaphoreTake(spi_sem, 40) != pdPASS) {
                EL_LOGW("\n\n\n\n [spi dma write timeout]\n\n\n\n");
                xSemaphoreGive(spi_sem);
                continue;
            }

            send_seq++;
            at.sent_len += send_len;
            at.port->spi_write(write_done, sizeof(write_done));

            time = xTaskGetTickCount();
            do { 
                hx_drv_gpio_get_in_value(AON_GPIO0, &value);
                if (xTaskGetTickCount() > time + 10) break;
            } while (value == 1);
            EL_LOGD("wrote %d/%d", at.sent_len, at.tbuf_len);

            if (at.sent_len < at.tbuf_len) {
                at_requset_write(&at);
            } else {
                at.sent_len = 0;
                at.tbuf_len = 0;
            }
        } else {
            EL_LOGW("unknown direct: %d", at.ctrl.direct);
            continue;
        }
    }
}

static void at_recv_parser(void* arg) {
    char str[512] = {0};
    char *ptr = NULL;
    uint32_t len = 0, i = 0;
    uint32_t num = sizeof(resp_flow) / sizeof(resp_flow[0]);
    while (1) {
        if (ulTaskNotifyTake(pdFALSE, portMAX_DELAY) > 0) {
            len = at_rbuf->extract('\n', str, sizeof(str));
            str[len] = '\0';
            ptr = (str[0] == '>') ? str + 1 : str;
            for (i = 0; i < num; i++) {
                if (strncmp(ptr, resp_flow[i].str, strlen(resp_flow[i].str)) == 0) {
                    resp_flow[i].act(ptr, arg);
                    fail_count = 0; // got response, reset fail count
                    EL_LOGI("<- %s", ptr);
                    break;
                }
            }
            if (i == num && len > 2) {
                EL_LOGI("<- %s\n", ptr);
            }
            memset(str, 0, len);
        }
    }
}

static void network_status_handler(void* arg) {
    uint32_t value  = 0;
    edgelab::NetworkWE2* net = (edgelab::NetworkWE2*)arg;
    while (1) {
        if (xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &value, portMAX_DELAY) == pdPASS) {
            // switch (value)
            // {
            // case NETWORK_JOINED: 
            //     sprintf(at.tbuf, AT_STR_HEADER AT_STR_CIPSTA "?" AT_STR_CRLF);
            //     at_send(&at, AT_SHORT_TIME_MS);
            // default:
            //     break;
            // }
            net->set_status((el_net_sta_t)value);
        }
    }
}

// static void logger(void* arg) {
//     char pcWriteBuffer[512] = {0};
//     while (1) {
        // vTaskList(pcWriteBuffer);
        // el_printf("\nName                           State Priority Stack Num\n");
        // el_printf("%s", pcWriteBuffer);
        // vTaskGetRunTimeStats(pcWriteBuffer);
        // el_printf("\nTask Name                         Run Time  CPU Usage\n");
        // el_printf("%s", pcWriteBuffer);
//         el_printf(" >_< \n");
//         vTaskDelay(15 * 1000);
//     }
// }

static void at_base_init(NetworkWE2* ptr) {
    EL_LOGD("at_base_init\n");

    at_rbuf = new lwRingBuffer(AT_RX_MAX_LEN);
    at_got_response = xSemaphoreCreateBinary();
    pubraw_complete = xSemaphoreCreateBinary();

    spi_sem = xSemaphoreCreateBinary();

    EL_ASSERT(at_rbuf);
    EL_ASSERT(at_got_response);
    EL_ASSERT(pubraw_complete);
    at.rbuf = at_rbuf;
    // Parse data and trigger events
    if (xTaskCreate(at_recv_parser, 
                    "at_recv_parser", 
                    CONFIG_EL_NETWORK_STACK_SIZE, 
                    ptr, 
                    CONFIG_EL_NETWORK_PRIO, 
                    &at_rx_parser) != pdPASS) {
        EL_LOGD("at_recv_parser create error\n");
        return;
    }
    // Handle network status change events
    if (xTaskCreate(network_status_handler, 
                    "network_status_handler", 
                    CONFIG_EL_NETWORK_STATUS_STACK_SIZE, 
                    ptr, 
                    CONFIG_EL_NETWORK_STATUS_PRIO, 
                    &status_handler) != pdPASS) {
        EL_LOGD("network_status_handler create error\n");
        return;
    }
    if (xTaskCreate(spi_trans_task, 
                    "spi_trans_task", 
                    512, 
                    NULL, 
                    CONFIG_EL_NETWORK_PRIO, 
                    &spi_trans_ctrl) != pdPASS) {
        EL_LOGD("spi_trans_task create error\n");
        return;
    }
    // xTaskCreate(logger, "logger", 384, NULL, 5, NULL);
    
    at.state = AT_STATE_IDLE;
}

namespace edgelab {

void NetworkWE2::init(status_cb_t cb) {
    el_err_code_t err = EL_OK;
    _at = &at;
    if (at.state == AT_STATE_LOST) {
        at_base_init(this);
        if (at.state != AT_STATE_IDLE) return;
    }
    if (at.port == NULL) {
        at_port_init(&at);
        if (at.port == NULL) return;
    }

    xSemaphoreGive(pubraw_complete);
    at_port_flush(&at);

    sprintf(at.tbuf, AT_STR_ECHO AT_STR_CRLF);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGW("AT ECHO ERROR : %d\n", err);
        return;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_CWMODE "=1" AT_STR_CRLF);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGW("AT CWMODE ERROR : %d\n", err);
        return;
    }
    
    if (cb) this->status_cb = cb;
    EL_LOGI("network init ok\n");
    this->_time_synced = false;
    this->set_status(NETWORK_IDLE);
}

void NetworkWE2::deinit() {
    vTaskDelete(at_rx_parser);
    vTaskDelete(status_handler);
#ifdef CONFIG_EL_NETWORK_SPI_AT
    vTaskDelete(spi_trans_ctrl);
    at.port->spi_close();
#else
    at.port->uart_close();
#endif
    delete at_rbuf;
    at.port = NULL;
    at.state = AT_STATE_LOST;
    this->set_status(NETWORK_LOST);
}

el_err_code_t NetworkWE2::join(const char* ssid, const char* pwd) {
    el_err_code_t err = EL_OK;
    // EL_LOGD("join %s %s\n", ssid, pwd);
    if (this->network_status == NETWORK_JOINED || this->network_status == NETWORK_CONNECTED) {
        return EL_OK;
    } else if (network_status == NETWORK_LOST) {
        return EL_EPERM;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_CWJAP "=\"%s\",\"%s\"" AT_STR_CRLF, ssid, pwd);
    err = at_send(&at, AT_LONG_TIME_MS * 4);
    if (err != EL_OK) {
        EL_LOGW("AT CWJAP ERROR : %d\n", err);
        return err;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_CIPSTA "?" AT_STR_CRLF);
    at_send(&at, AT_SHORT_TIME_MS);

    return EL_OK;
}

el_err_code_t NetworkWE2::quit() {
    el_err_code_t err = EL_OK;
    if (this->network_status == NETWORK_IDLE || this->network_status == NETWORK_LOST) {
        return EL_OK;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_CWQAP AT_STR_CRLF);
    err = at_send(&at, AT_LONG_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT CWJAP ERROR : %d\n", err);
        return err;
    }
    return EL_OK;
}

el_err_code_t NetworkWE2::set_mdns(mdns_record_t record) {
    el_err_code_t err = EL_OK;
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MDNSSTART "=\"%s\"" AT_STR_CRLF, 
            record.host_name);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MDNS ERROR : %d\n", err);
        return err;
    }

    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MDNSADD "=\"%s\",\"%s\"" AT_STR_CRLF, 
            MDNS_ITEM_SERVER, record.server);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MDNS ADD %s ERROR : %d\n", MDNS_ITEM_SERVER, err);
        return err;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MDNSADD "=\"%s\",\"%d\"" AT_STR_CRLF,
            MDNS_ITEM_PORT, record.port);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MDNS ADD %s ERROR : %d\n", MDNS_ITEM_PORT, err);
        return err;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MDNSADD "=\"%s\",\"%s\"" AT_STR_CRLF,
            MDNS_ITEM_PROTOCAL, record.protocol);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MDNS ADD %s ERROR : %d\n", MDNS_ITEM_PROTOCAL, err);
        return err;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MDNSADD "=\"%s\",\"%s\"" AT_STR_CRLF,
            MDNS_ITEM_DEST, record.destination);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MDNS ADD %s ERROR : %d\n", MDNS_ITEM_DEST, err);
        return err;
    }
    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MDNSADD "=\"%s\",\"%s\"" AT_STR_CRLF,
            MDNS_ITEM_AUTH, record.authentication);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MDNS ADD %s ERROR : %d\n", MDNS_ITEM_AUTH, err);
        return err;
    }

    return EL_OK;
}

el_err_code_t NetworkWE2::connect(const mqtt_server_config_t mqtt_cfg, topic_cb_t cb) {
    el_err_code_t err = EL_OK;
    if (this->network_status == NETWORK_CONNECTED) {
        return EL_OK;
    } else if (this->network_status != NETWORK_JOINED) {
        return EL_EPERM;
    }

    if (cb == NULL) {
        return EL_EINVAL;
    }
    at.cb = cb;
    this->topic_cb = cb;

    if (mqtt_cfg.use_ssl) {
        if (!this->_time_synced) {
            sprintf(at.tbuf, AT_STR_HEADER AT_STR_CIPSNTPCFG "=1,%d,\"%s\",\"%s\"" AT_STR_CRLF,
                    UTC_TIME_ZONE_CN, SNTP_SERVER_CN, SNTP_SERVER_US);
            EL_LOGI("AT CIPSNTPCFG : %s\n", at.tbuf);
            at_port_write(&at);
            uint32_t t = 0;
            while (!this->_time_synced) {
                if (t >= AT_LONG_TIME_MS * 12) {
                    EL_LOGI("AT CIPSNTPCFG TIMEOUT\n");
                    return EL_ETIMOUT;
                }
                el_sleep(100);
                t += 100;
            }
        }
        sprintf(at.tbuf, AT_STR_HEADER AT_STR_CIPSNTPTIME AT_STR_CRLF);
        err = at_send(&at, AT_SHORT_TIME_MS);
        if (err != EL_OK) {
            EL_LOGI("AT CIPSNTPTIME ERROR : %d\n", err);
            return err;
        }
    }

    sprintf(at.tbuf,
            AT_STR_HEADER AT_STR_MQTTUSERCFG "=0,%d,\"%s\",\"%s\",\"%s\",0,0,\"\"" AT_STR_CRLF,
            mqtt_cfg.use_ssl ? 4 : 1,
            mqtt_cfg.client_id,
            mqtt_cfg.username,
            mqtt_cfg.password);
    EL_LOGD("AT MQTTUSERCFG : %s\n", at.tbuf);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGI("AT MQTTUSERCFG ERROR : %d\n", err);
        this->disconnect();
        return err;
    }

    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MQTTCONN "=0,\"%s\",%d,1" AT_STR_CRLF, 
            mqtt_cfg.address,
            mqtt_cfg.port);
    err = at_send(&at, AT_LONG_TIME_MS * 12);
    if (err != EL_OK) {
        EL_LOGI("AT MQTTCONN ERROR : %d\n", err);
        this->disconnect();
        return err;
    }
    this->set_status(NETWORK_CONNECTED);

    return EL_OK;
}

el_err_code_t NetworkWE2::disconnect() {
    el_err_code_t err = EL_OK;
    if (this->network_status != NETWORK_CONNECTED) {
        return EL_EPERM;
    }

    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MQTTCLEAN "=0" AT_STR_CRLF);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MQTTCLEAN ERROR : %d\n", err);
        return err;
    }
    return EL_OK;
}

el_err_code_t NetworkWE2::subscribe(const char* topic, mqtt_qos_t qos) {
    el_err_code_t err = EL_OK;
    if (this->network_status != NETWORK_CONNECTED) {
        return EL_EPERM;
    }

    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MQTTSUB "=0,\"%s\",%d" AT_STR_CRLF, topic, qos);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MQTTSUB ERROR : %d\n", err);
        return err;
    }
    return EL_OK;
}

el_err_code_t NetworkWE2::unsubscribe(const char* topic) {
    el_err_code_t err = EL_OK;
    if (this->network_status != NETWORK_CONNECTED) {
        return EL_EPERM;
    }

    sprintf(at.tbuf, AT_STR_HEADER AT_STR_MQTTUNSUB "=0,\"%s\"" AT_STR_CRLF, topic);
    err = at_send(&at, AT_SHORT_TIME_MS);
    if (err != EL_OK) {
        EL_LOGD("AT MQTTUNSUB ERROR : %d\n", err);
        return err;
    }
    return EL_OK;
}

el_err_code_t NetworkWE2::publish(const char* topic, const char* dat, uint32_t len, mqtt_qos_t qos) {
    el_err_code_t err = EL_OK;
    if (this->network_status != NETWORK_CONNECTED) {
        return EL_EPERM;
    }

    if (fail_count >= 5) {
        at_port_flush(&at);
        if (at.state == AT_STATE_PROCESS) {
            this->deinit();
            return EL_FAILED;
        }
        fail_count = 0;
    }

    if (len + strlen(topic) < 200) {
        char special_chars[] = "\\\"\,\n\r";
        char buf[230] = {0};
        uint8_t j = 0;
        for (uint8_t i = 0; i < len; i++) {
            if (strchr(special_chars, dat[i]) != NULL) {
                buf[j++] = '\\';
            }
            buf[j++] = dat[i];
        }
        buf[j] = '\0';
        sprintf(at.tbuf, AT_STR_HEADER AT_STR_MQTTPUB "=0,\"%s\",\"%s\",%d,0" AT_STR_CRLF, topic, buf, qos);
        err = at_send(&at, AT_SHORT_TIME_MS);
        if (err != EL_OK) {
            EL_LOGD("AT MQTTPUB ERROR : %d\n", err);
            return err;
        }
    } 
    else if (len + 1 < sizeof(at.tbuf)) {
        if (xSemaphoreTake(pubraw_complete, 6 * AT_SHORT_TIME_MS * portTICK_PERIOD_MS) != pdTRUE) {
            xSemaphoreGive(pubraw_complete);
            EL_LOGW("AT MQTTPUB ERROR : PUBRAW TIMEOUT\n");
            fail_count++;
            return EL_ETIMOUT;
        }
        sprintf(at.tbuf, AT_STR_HEADER AT_STR_MQTTPUB "RAW=0,\"%s\",%d,%d,0" AT_STR_CRLF, topic, len, qos);
        err = at_send(&at, AT_SHORT_TIME_MS);
        if (err != EL_OK) {
            xSemaphoreGive(pubraw_complete);
            EL_LOGW("AT MQTTPUB ERROR : %d\n", err);
            fail_count++;
            return err;
        }
        snprintf(at.tbuf, len + 1, "%s", dat);
        at.sent_len = 0;
        at.tbuf_len = strlen(at.tbuf);
#ifdef CONFIG_EL_NETWORK_SPI_AT
        at_port_write(&at);
#else
        send_len = at.tbuf_len > AT_DMA_TRAN_MAX_SIZE ? AT_DMA_TRAN_MAX_SIZE : at.tbuf_len;
        at.port->uart_write_udma(at.tbuf, send_len, (void*)at_port_txcb);
#endif
    } else {
        EL_LOGW("AT MQTTPUB ERROR : DATA TOO LONG\n");
        return EL_ENOMEM;
    }

    return EL_OK;
}

}  // namespace edgelab

/************* AT port driver callbacks **************/
#ifdef CONFIG_EL_NETWORK_SPI_AT
static void spi_dma_cb(void* status) {
    BaseType_t taskwaken = pdFALSE;
    xSemaphoreGiveFromISR(spi_sem, &taskwaken);
    portYIELD_FROM_ISR(taskwaken);
}
static void at_port_handshake_cb(uint8_t group, uint8_t aIndex) {
    BaseType_t taskwaken = pdFALSE;
    xTaskNotifyFromISR(spi_trans_ctrl, AT_SPI_HANDSHAKE_FLAG, eNoAction, &taskwaken);
    portYIELD_FROM_ISR(taskwaken);
}
#else
static void at_port_txcb(void* arg) {
    BaseType_t taskwaken = pdFALSE;
    send_len = (at.tbuf_len - at.sent_len) > AT_DMA_TRAN_MAX_SIZE ? AT_DMA_TRAN_MAX_SIZE : (at.tbuf_len - at.sent_len);
    at.sent_len += send_len;
    // EL_LOGD("PUBRAW: %u / %u", at.sent_len, at.tbuf_len);
    if (at.sent_len < at.tbuf_len) {
        send_len = (at.tbuf_len - at.sent_len) > AT_DMA_TRAN_MAX_SIZE ? AT_DMA_TRAN_MAX_SIZE : (at.tbuf_len - at.sent_len);
        at.port->uart_write_udma(at.tbuf + at.sent_len, send_len, (void*)at_port_txcb);
    }
}
static void at_port_rxcb(void* arg) {
    BaseType_t taskwaken = pdFALSE;
    if (at.state == AT_STATE_LOST) {
        at.port->uart_read_udma(dma_rx, 1, (void*)at_port_rxcb);
        return;
    }
    *at_rbuf << dma_rx[0];
    if (dma_rx[0] == '\n') {
        vTaskNotifyGiveFromISR(at_rx_parser, &taskwaken);
    }
    at.port->uart_read_udma(dma_rx, 1, (void*)at_port_rxcb);
    portYIELD_FROM_ISR(taskwaken);
}
#endif

/************* AT command response actions functions **************/
void resp_action_ok(const char* resp, void* arg) {
    EL_LOGD("OK\n");
    at.state = AT_STATE_OK;
    xSemaphoreGive(at_got_response);
}
void resp_action_error(const char* resp, void* arg) {
    EL_LOGD("ERROR\n");
    at.state = AT_STATE_ERROR;
    xSemaphoreGive(at_got_response);
}
void resp_action_ready(const char* resp, void* arg) {
    EL_LOGD("READY\n");
    at.state = AT_STATE_READY;
    xSemaphoreGive(at_got_response);
}
void resp_action_wifi(const char* resp, void* arg) {
    if (resp[strlen(AT_STR_RESP_WIFI_H)] == 'G') {
        EL_LOGD("WIFI CONNECTED\n");
        xTaskNotify(status_handler, NETWORK_JOINED, eSetValueWithOverwrite);
    } else if (resp[strlen(AT_STR_RESP_WIFI_H)] == 'D') {
        EL_LOGD("WIFI DISCONNECTED\n");
        xTaskNotify(status_handler, NETWORK_IDLE, eSetValueWithOverwrite);
    }
}
void resp_action_mqtt(const char* resp, void* arg) {
    edgelab::NetworkWE2* net = (edgelab::NetworkWE2*)arg;
    if (resp[strlen(AT_STR_RESP_MQTT_H)] == 'C') {
        EL_LOGD("MQTT CONNECTED\n");
        xTaskNotify(status_handler, NETWORK_CONNECTED, eSetValueWithOverwrite);
    } else if (resp[strlen(AT_STR_RESP_MQTT_H)] == 'D') {
        EL_LOGD("MQTT DISCONNECTED\n");
        xTaskNotify(status_handler, NETWORK_IDLE, eSetValueWithOverwrite);
    } else if (resp[strlen(AT_STR_RESP_MQTT_H)] == 'S') {
        EL_LOGI("MQTT SUBRECV\n");
        int topic_len = 0, msg_len = 0, str_len = 0;

        char* topic_pos = strchr(resp, '"');
        if (topic_pos == NULL) {
            EL_LOGW("MQTT SUBRECV TOPIC ERROR\n");
            return;
        }
        topic_pos++;  // Skip start character
        while (topic_pos[str_len] != '"') {
            str_len++;
        }
        topic_len = str_len;

        str_len = 0;
        char* msg_pos = topic_pos + topic_len + 1;
        if (msg_pos[0] != ',') {
            EL_LOGW("MQTT SUBRECV MSG ERROR\n");
            return;
        }
        msg_pos++;  // Skip start character
        while (msg_pos[str_len] != ',') {
            str_len++;
        }
        for (int i = 0; i < str_len; i++) {
            if (msg_pos[i] < '0' || msg_pos[i] > '9') {
                EL_LOGW("MQTT SUBRECV MSG ERROR\n");
                return;
            }
            msg_len = msg_len * 10 + (msg_pos[i] - '0');
        }
        msg_pos += str_len + 1;

        if (net->topic_cb) net->topic_cb(topic_pos, topic_len, msg_pos, msg_len);
        return;
    }
}
void resp_action_ip(const char* resp, void* arg) {
    edgelab::NetworkWE2* net = (edgelab::NetworkWE2*)arg;
    uint32_t ofs = strlen(AT_STR_RESP_IP_H);
    if (strncmp(resp + ofs, "ip:", 3) == 0) {
        ofs += 3;
        net->_ip.ip = ipv4_addr_t::from_str(std::string(resp + ofs, strlen(resp + ofs)));
        return;
    } else if (strncmp(resp + ofs, "gateway:", 8) == 0) {
        ofs += 8;
        net->_ip.gateway = ipv4_addr_t::from_str(std::string(resp + ofs, strlen(resp + ofs)));
        return;
    } else if (strncmp(resp + ofs, "netmask:", 8) == 0) {
        ofs += 8;
        net->_ip.netmask = ipv4_addr_t::from_str(std::string(resp + ofs, strlen(resp + ofs)));
        return;
    }
}
void resp_action_ntp(const char* resp, void* arg) {
    edgelab::NetworkWE2* net = (edgelab::NetworkWE2*)arg;
    EL_LOGD("NTP TIME UPDATED!\n");
    net->_time_synced = true;
    return;
}
void resp_action_pubraw(const char* resp, void* arg) {
    // EL_LOGI("AT PUBRAW RESP: %u ms\n", xTaskGetTickCount() - last_pub);
    // last_pub = xTaskGetTickCount();
    xSemaphoreGive(pubraw_complete);
}

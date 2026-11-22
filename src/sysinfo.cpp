/* 
 *  sysinfo.cpp is part of the HB-RF-ETH firmware - https://github.com/alexreinert/HB-RF-ETH
 *  
 *  Copyright 2022 Alexander Reinert
 *  
 *  The HB-RF-ETH firmware is licensed under a
 *  Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.
 *  
 *  You should have received a copy of the license along with this
 *  work.  If not, see <http://creativecommons.org/licenses/by-nc-sa/4.0/>.
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *  
 */

#include "sysinfo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_adc_cal.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pins.h"

#define DEFAULT_VREF 1100

static const char *TAG = "SysInfo";

static double _cpuUsage;
static char _serial[13];
static const char *_currentVersion;
static board_type_t _board;
static const char *_resetReason = "unknown";
static uint32_t _bootCount = 0;

// RTC memory survives a software reset, but not a power cycle. It is used to
// tell a restart requested through the WebUI apart from any other software reset.
#define RESTART_REQUEST_MAGIC 0x48425245UL
static RTC_NOINIT_ATTR uint32_t _restartRequest;

static const char *resetReasonToString(esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    default:
        return "unknown";
    }
}

static uint32_t incrementBootCount()
{
    uint32_t count = 0;
    nvs_handle_t handle;

    // NVS is initialized by Settings, which is created before SysInfo
    if (nvs_open("sysinfo", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_get_u32(handle, "bootCount", &count);
        count++;
        nvs_set_u32(handle, "bootCount", count);
        nvs_commit(handle);
        nvs_close(handle);
    }

    return count;
}

void updateCPUUsageTask(void *arg)
{
    TaskStatus_t *taskStatus = (TaskStatus_t *)malloc(25 * sizeof(TaskStatus_t));

    TaskHandle_t idle0Task = xTaskGetIdleTaskHandleForCPU(0);
    TaskHandle_t idle1Task = xTaskGetIdleTaskHandleForCPU(1);

    uint32_t totalRunTime = 0, idleRunTime = 0, lastTotalRunTime = 0, lastIdleRunTime = 0;

    for (;;)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        UBaseType_t taskCount = uxTaskGetSystemState(taskStatus, 25, &totalRunTime);

        idleRunTime = 0;

        if (totalRunTime > 0)
        {
            for (int i = 0; i < taskCount; i++)
            {
                TaskStatus_t ts = taskStatus[i];

                if (ts.xHandle == idle0Task || ts.xHandle == idle1Task)
                {
                    idleRunTime += ts.ulRunTimeCounter;
                }
            }
        }

        _cpuUsage = 100.0 - ((idleRunTime - lastIdleRunTime) * 100.0 / ((totalRunTime - lastTotalRunTime) * 2));

        lastIdleRunTime = idleRunTime;
        lastTotalRunTime = totalRunTime;
    }

    free(taskStatus);
    vTaskDelete(NULL);
}

uint32_t get_voltage(adc_unit_t adc_unit, adc_channel_t adc_channel, adc_bits_width_t adc_width, adc_atten_t adc_atten)
{
    esp_adc_cal_characteristics_t *adc_chars = reinterpret_cast<esp_adc_cal_characteristics_t *>(calloc(1, sizeof(esp_adc_cal_characteristics_t)));
    esp_adc_cal_characterize(adc_unit, adc_atten, adc_width, DEFAULT_VREF, adc_chars);

    adc_gpio_init(adc_unit, adc_channel);

    if (adc_unit == ADC_UNIT_1)
    {
        adc1_config_width(adc_width);
        adc1_config_channel_atten((adc1_channel_t)adc_channel, adc_atten);
    }
    else
    {
        adc2_config_channel_atten((adc2_channel_t)adc_channel, adc_atten);
    }

    uint32_t voltage;
    esp_adc_cal_get_voltage(adc_channel, adc_chars, &voltage);

    free(adc_chars);

    return voltage;
}

board_type_t detectBoard()
{
    uint32_t voltage = get_voltage(BOARD_REV_SENSE_UNIT, BOARD_REV_SENSE_CHANNEL, ADC_WIDTH_BIT_10, ADC_ATTEN_DB_11);

    switch (voltage) // R31/R32
    {
    case 400 ... 700: // 10K/2K
        return BOARD_TYPE_REV_1_10_PUB;

    case 1500 ... 1800: // 10K/10K
        return BOARD_TYPE_REV_1_8_SK;

    case 2600 ... 2900: // 2K/10K
        return BOARD_TYPE_REV_1_8_PUB;

    case 2901 ... 3200: // 1K/12K
        return BOARD_TYPE_REV_1_10_SK;

    default:
        ESP_LOGW(TAG, "Could not determine board, voltage: %u", voltage);
        return BOARD_TYPE_UNKNOWN;
    }
}

SysInfo::SysInfo()
{
    xTaskCreate(updateCPUUsageTask, "UpdateCPUUsage", 4096, NULL, 3, NULL);

    uint8_t baseMac[6];
    esp_read_mac(baseMac, ESP_MAC_ETH);
    snprintf(_serial, sizeof(_serial), "%02X%02X%02X%02X%02X%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);

    _currentVersion = esp_ota_get_app_description()->version;

    _board = detectBoard();

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_SW && _restartRequest == RESTART_REQUEST_MAGIC)
    {
        _resetReason = "webui";
    }
    else
    {
        _resetReason = resetReasonToString(reason);
    }
    _restartRequest = 0;

    _bootCount = incrementBootCount();

    ESP_LOGI(TAG, "Reset reason: %s, boot count: %u", _resetReason, _bootCount);
}

double SysInfo::getCpuUsage()
{
    return _cpuUsage;
}

double SysInfo::getMemoryUsage()
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    return 100.0 - (info.total_free_bytes * 100.0 / (info.total_free_bytes + info.total_allocated_bytes));
}

const char *SysInfo::getSerialNumber()
{
    return _serial;
}

board_type_t SysInfo::getBoardType()
{
    return _board;
}

const char *SysInfo::getCurrentVersion()
{
    return _currentVersion;
}

const char *SysInfo::getResetReason()
{
    return _resetReason;
}

uint32_t SysInfo::getBootCount()
{
    return _bootCount;
}

void SysInfo::restart()
{
    _restartRequest = RESTART_REQUEST_MAGIC;
    esp_restart();
}

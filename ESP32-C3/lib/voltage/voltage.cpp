#include "voltage.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

namespace voltage {

    static const char *TAG = "voltage";
    static adc_oneshot_unit_handle_t s_adcHandle = nullptr;
    static adc_cali_handle_t s_caliHandle = nullptr;
    static const adc_channel_t kNoChannel = static_cast<adc_channel_t>(-1);
    static adc_channel_t s_channel = kNoChannel;


    void init(gpio_num_t pin) {
        // ESP32-C3: GPIO0-GPIO4 map to ADC1 channels 0-4
        if (pin < GPIO_NUM_0 || pin > GPIO_NUM_4) {
            ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin on ESP32-C3", static_cast<int>(pin));
            return;
        }
        s_channel = static_cast<adc_channel_t>(pin);

        if (s_adcHandle != nullptr) {
            adc_oneshot_del_unit(s_adcHandle);
            s_adcHandle = nullptr;
        }
        if (s_caliHandle != nullptr) {
            adc_cali_delete_scheme_curve_fitting(s_caliHandle);
            s_caliHandle = nullptr;
        }

        const adc_oneshot_unit_init_cfg_t initCfg = {
            .unit_id = ADC_UNIT_1,
        };
        const esp_err_t adcInitRet = adc_oneshot_new_unit(&initCfg, &s_adcHandle);
        if (adcInitRet != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(adcInitRet));
            s_adcHandle = nullptr;
            return;
        }

        const adc_oneshot_chan_cfg_t chanCfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        const esp_err_t chanRet = adc_oneshot_config_channel(s_adcHandle, s_channel, &chanCfg);
        if (chanRet != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(chanRet));
            adc_oneshot_del_unit(s_adcHandle);
            s_adcHandle = nullptr;
            return;
        }

        const adc_cali_curve_fitting_config_t caliCfg = {
            .unit_id = ADC_UNIT_1,
            .chan = s_channel,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };

        const esp_err_t caliRet = adc_cali_create_scheme_curve_fitting(&caliCfg, &s_caliHandle);

        if (caliRet != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration unavailable (%s); using linear fallback", esp_err_to_name(caliRet));
            s_caliHandle = nullptr;
        }
        ESP_LOGI(TAG, "ADC init OK on GPIO%d (ch%d)", static_cast<int>(pin), static_cast<int>(s_channel));
    }

    float read_voltage() {
        if (s_adcHandle == nullptr || s_channel == kNoChannel) {
            return 0.0f;
        }

        static constexpr int kSamples = 16;
        int32_t sum = 0;
        for (int i = 0; i < kSamples; ++i) {
            int raw = 0;
            const esp_err_t readRet = adc_oneshot_read(s_adcHandle, s_channel, &raw);
            if (readRet != ESP_OK) {
                ESP_LOGE(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(readRet));
                return 0.0f;
            }
            sum += raw;
        }
        const int avgRaw = static_cast<int>(sum / kSamples);

        int millivolts = 0;

        if (s_caliHandle != nullptr) {
            const esp_err_t caliReadRet = adc_cali_raw_to_voltage(s_caliHandle, avgRaw, &millivolts);
            if (caliReadRet != ESP_OK) {
                ESP_LOGW(TAG, "adc_calib_raw_to_voltage failed: %s", esp_err_to_name(caliReadRet));
                millivolts = (avgRaw * 3100) / 4095;
            }
        } else {
            // Linear fallback: 12-bit ADC, ~3.1V full-scale with 12dB attenuation
            millivolts = (avgRaw * 3100) / 4095;
        }

        // Apply voltage divider ratio (equal resistors = x2)
        return (millivolts / 1000.0f) * 2.0f;
    }

}  // namespace voltage

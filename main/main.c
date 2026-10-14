// main/main.c —— DeepSeek Passport 应用入口。
//
// 职责:初始化 BSP、把按键事件送进队列、启动应用界面、启动串口控制台。
// 界面本身在 passport_app.c,宠物角色在 passport_pet.c。
//
// 按键语义(当前):
//   确定 短按   宠物回应(开心一阵)
//   确定 长按   待机预览开/关(真正的电源管理属于后续里程碑)
//   上/下 短按  切换宠物表情,便于用串口控制台逐一核对素材
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "key_guard.h"
#include "nvs_flash.h"
#include "passport_app.h"
#include "passport_chat.h"
#include "passport_cli.h"
#include "passport_config_store.h"
#include "passport_notify.h"
#include "passport_pet_assets.h"
#include "passport_work_client.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            passport_app_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    // Sized for what a key press can end up doing, not for the queue plumbing. Holding UP
    // builds the setup screen, and building it encodes a QR symbol, which costs roughly
    // six kilobytes of stack between the encoder's working copy and the builder's module
    // map. At 4096 the press overflowed the stack and the device rebooted, which looked
    // like the access point switching itself off.
    if (xTaskCreate(input_task, "passport_input", 12288, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later, registers a press that
// is already down, and grades it a long press once it has been down for its
// threshold (500 ms here), so an unintended action fires almost immediately. Drop
// key actions until that press comes up; the deadline is only a safety net so a
// stuck key cannot leave the device dead. The state machine lives in key_guard.h so
// it can be unit-tested without the rest of the application.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

// Injected events from the serial console enter the same queue as a physical
// press, so `ui key` drives the application exactly like a finger does.
static void passport_cli_inject(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "DeepSeek Passport 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    // 按键唤醒时,那颗键在整个启动期间还按着。先挡住它的动作(见 on_key)。
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // Configuration lives in NVS. A failure here does not stop the device, and it
    // never triggers an erase: falling back to defaults and saying so is better
    // than destroying whatever else the partition holds.
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS 需要格式化(%s);不自动擦除,本次使用默认配置", esp_err_to_name(nvs_err));
    } else if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_err));
    }
    bool cfg_loaded = false;
    if (passport_config_store_init(&cfg_loaded) != ESP_OK) {
        ESP_LOGW(TAG, "配置存储不可用;本次运行使用默认配置");
    }

    // 屏幕是应用唯一的界面载体,失败就没有可用的产品形态——打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);
    // Then let the stored preference win. 0 means off, which is why the console
    // command that sets it is documented as the way back.
    bsp_display_backlight(passport_config_current()->brightness);

    // 其余外设单项失败不阻塞:应用照常启动,只是相关功能不可用。
    const esp_err_t input_err = input_dispatch_init();
    const esp_err_t button_err = input_err == ESP_OK
                               ? bsp_button_init(on_key, NULL)
                               : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    } else if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。初始化完成后按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }
    const bool audio_ok = (bsp_audio_init() == ESP_OK);
    const bool battery_ok = (bsp_battery_init() == ESP_OK);

    const esp_err_t app_err = passport_app_start();
    if (app_err != ESP_OK) {
        ESP_LOGE(TAG, "应用界面启动失败: %s", esp_err_to_name(app_err));
    }
    s_input_ready = true;

    // The serial console is a development and inspection surface. If it cannot
    // start, the device keeps working; it is not a required subsystem.
    passport_cli_set_key_sink(passport_cli_inject);
    if (passport_cli_start() != ESP_OK) {
        ESP_LOGW(TAG, "串口控制台不可用;设备其余功能不受影响");
    }

    // Conversation worker. It owns the stack a TLS handshake needs, so the button and
    // console tasks never have to host network work.
    if (passport_chat_start() != ESP_OK) {
        ESP_LOGE(TAG, "对话线程启动失败;按 OK 将无法提问");
    }

    // Work mode's bridge client. Its own task, because polling and posting are network work.
    if (passport_work_client_start() != ESP_OK) {
        ESP_LOGE(TAG, "工作模式轮询启动失败;工作状态不会自动更新");
    }

    // The chime buffer, allocated once before the heap fragments. A notification that fails to
    // allocate would be silent for a reason nobody could see.
    if (passport_notify_init() != ESP_OK) {
        ESP_LOGW(TAG, "提示音不可用;工作模式的状态变化将只上屏不发声");
    }

    ESP_LOGI(TAG, "就绪:Display=1 Button=%d Audio=%d Battery=%d App=%d Cfg=%d",
             button_err == ESP_OK, audio_ok, battery_ok, app_err == ESP_OK, cfg_loaded);

    // Forced reference: the pet sprites live in flash and LVGL draws them, so
    // nothing links them until a screen uses one. Touching the descriptor and its
    // pixel map keeps the asset object in the image (--gc-sections drops
    // unreferenced data) and proves the descriptors are real at runtime.
    ESP_LOGI(TAG, "宠物素材: %d 帧 %dx%d, 首帧 \"%s\" %u B, 共 %u B flash",
             PASSPORT_PET_FRAME_COUNT, PASSPORT_PET_W, PASSPORT_PET_H,
             passport_pet_frame_names[PASSPORT_PET_FRAME_IDLE_A],
             (unsigned)passport_pet_frames[PASSPORT_PET_FRAME_IDLE_A]->data_size,
             (unsigned)(PASSPORT_PET_FRAME_COUNT * PASSPORT_PET_W * PASSPORT_PET_H * 3));
}

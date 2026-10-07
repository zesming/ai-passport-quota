#include "quota_service.h"
#include "quota_portable_service.h"
#include "quota_store.h"
#include "quota_usb.h"

#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "quota_service";

#define EVENT_QUEUE_DEPTH 16
#define NETWORK_TASK_STACK 10240
#define NETWORK_TASK_PRIORITY 5
#define WIFI_RETRY_MIN_MS 2000
#define WIFI_RETRY_MAX_MS 30000

typedef struct {
    bool sleeping;
    uint32_t generation;
} display_scheduler_t;

typedef struct {
    bool sleeping;
    uint32_t generation;
} display_state_t;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_network_task;
static atomic_bool s_pairing_requested;
static portMUX_TYPE s_display_state_mux = portMUX_INITIALIZER_UNLOCKED;
static display_scheduler_t s_display_scheduler = {.generation = 1};
static quota_service_view_t s_view;
static bool s_nvs_ready;
static bool s_wifi_stack_ready;
static bool s_wifi_initialized;
static bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_handlers_registered;
static bool s_wifi_retry_pending;
static uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
static int64_t s_wifi_retry_at_ms;
static uint32_t s_config_generation;
static bool s_pairing_screen_open;
static int64_t s_pairing_opened_at_ms;

static quota_frame_decoder_t *s_usb_decoder;
static atomic_uint_fast64_t s_usb_deadline;
static atomic_bool s_usb_io_busy;
static uint64_t s_usb_partial_at,s_usb_opener_at;
static char s_usb_session[QUOTA_USB_SESSION_BYTES+1],s_usb_opener[9];

static bool pairing_requested(void)
{
    return atomic_load(&s_pairing_requested);
}
static uint64_t monotonic_ms(void);
static uint64_t usb_deadline_ms(void) { return atomic_load(&s_usb_deadline); }
static bool usb_active(void) { return pairing_requested() && monotonic_ms()<usb_deadline_ms(); }
static bool usb_blocked(void) { return atomic_load(&s_usb_io_busy) || (pairing_requested() && !usb_deadline_ms()); }

static void mutex_lock(void)
{
    if (s_mutex != NULL) (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void mutex_unlock(void)
{
    if (s_mutex != NULL) (void)xSemaphoreGive(s_mutex);
}

static uint64_t current_epoch(void)
{
    time_t now = time(NULL);
    return now > 0 ? (uint64_t)now : 0;
}

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static display_state_t display_state_snapshot(void)
{
    display_state_t state;
    portENTER_CRITICAL(&s_display_state_mux);
    state.sleeping = s_display_scheduler.sleeping;
    state.generation = s_display_scheduler.generation;
    portEXIT_CRITICAL(&s_display_state_mux);
    return state;
}

static bool display_generation_is_current(uint32_t generation)
{
    bool current;
    portENTER_CRITICAL(&s_display_state_mux);
    current = !s_display_scheduler.sleeping &&
              s_display_scheduler.generation == generation;
    portEXIT_CRITICAL(&s_display_state_mux);
    return current;
}



/* Separate key: never change the existing device_cfg size/version/CRC contract. */
static void post_event(const quota_app_event_t *event, TickType_t wait)
{
    if (s_events != NULL && event != NULL) (void)xQueueSend(s_events, event, wait);
}

static void post_simple_event(quota_app_event_kind_t kind)
{
    quota_app_event_t event = {.kind = kind};
    post_event(&event, 0);
}

static bool pairing_active_locked(uint64_t now_ms)
{
    return quota_pairing_window_active(s_pairing_screen_open, now_ms,
                                       (uint64_t)s_pairing_opened_at_ms);
}

static bool prepare_network_stack(void)
{
    if (s_wifi_stack_ready) return true;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto failed;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto failed;

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&netif_config);
    if (s_sta_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    err = esp_netif_attach_wifi_station(s_sta_netif);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) goto failed;

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) goto failed;
    s_wifi_initialized = true;
    s_wifi_stack_ready = true;
    return true;

failed:
    ESP_LOGW(TAG, "Wi-Fi stack setup failed (%s)", esp_err_to_name(err));
    if (s_wifi_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    return false;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
#ifdef ESP_PLATFORM
    if (data) quota_portable_service_disconnected(((wifi_event_sta_disconnected_t *)data)->reason);
#endif
    if (id != WIFI_EVENT_STA_DISCONNECTED) return;
    bool sleeping = display_state_snapshot().sleeping;
    mutex_lock();
    bool was_connected = s_view.connected;
    s_view.connected = false;
    /* Keep disconnect facts even if stop failed; the sleeping worker never connects. */
    s_wifi_retry_pending = s_wifi_started;
    s_wifi_retry_at_ms = (int64_t)monotonic_ms() + (sleeping ? 0 : s_wifi_retry_delay_ms);
    if (!sleeping && s_wifi_retry_delay_ms < WIFI_RETRY_MAX_MS) {
        s_wifi_retry_delay_ms *= 2;
        if (s_wifi_retry_delay_ms > WIFI_RETRY_MAX_MS) {
            s_wifi_retry_delay_ms = WIFI_RETRY_MAX_MS;
        }
    }
    mutex_unlock();
    if (was_connected) post_simple_event(QUOTA_APP_EVENT_CONNECTION);
    if (!sleeping && s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;
    /* An old queued IP event can arrive after restart, before the new association. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    mutex_lock();
    if (!s_wifi_started || display_state_snapshot().sleeping) {
        mutex_unlock();
        return;
    }
    s_view.connected = true;
    s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
    s_wifi_retry_pending = false;
    mutex_unlock();
    post_simple_event(QUOTA_APP_EVENT_CONNECTION);
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static bool init_wifi(void)
{
    if (!prepare_network_stack()) return false;
    esp_err_t err;
    if (!s_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, &s_wifi_handler);
        if (err != ESP_OK) goto failed;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            ip_event_handler, NULL, &s_ip_handler);
        if (err != ESP_OK) {
            (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
            goto failed;
        }
        s_handlers_registered = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) goto failed;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) goto failed;
    }
    if (s_wifi_started) return true;
    err = esp_wifi_start();
    if (err != ESP_OK) goto failed;
    /* Restarting retains the RAM configuration and the provider deadline. */
    mutex_lock();
    s_wifi_started = true;
    s_wifi_retry_pending = true;
    s_wifi_retry_at_ms = (int64_t)monotonic_ms();
    mutex_unlock();
    return true;

failed:
    ESP_LOGW(TAG, "Wi-Fi init failed (%s)", esp_err_to_name(err));
    if (s_handlers_registered) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
            s_ip_handler);
        s_handlers_registered = false;
    }
    return false;
}

static void stop_wifi_for_sleep(void)
{
    if (!s_wifi_started) return;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi sleep stop failed (%s)", esp_err_to_name(err));
        return;
    }
    mutex_lock();
    s_wifi_started = false;
    bool was_connected = s_view.connected;
    s_view.connected = false;
    s_wifi_retry_pending = false;
    mutex_unlock();
    if (was_connected) post_simple_event(QUOTA_APP_EVENT_CONNECTION);
}

#ifdef ESP_PLATFORM
static bool portable_wifi_stop(void) { stop_wifi_for_sleep(); return !s_wifi_started; }
static void portable_notify(void) { post_simple_event(QUOTA_APP_EVENT_SNAPSHOT); }
static void portable_wake(void) { if (s_network_task) xTaskNotifyGive(s_network_task); }
#endif

#ifdef ESP_PLATFORM
static bool portable_try_lock(void) { return xSemaphoreTake(s_mutex,0)==pdTRUE; }
static uint32_t portable_config_generation_locked(void) { return s_config_generation; }
static void portable_config_changed_locked(void) { if(s_config_generation<UINT32_MAX)s_config_generation++; }
#endif
static void service_pairing_tick(bool sleeping);
static TickType_t network_wait(bool sleeping)
{
    uint64_t now=monotonic_ms();uint64_t deadline=sleeping&&!s_wifi_started?UINT64_MAX:now+500;
#ifdef ESP_PLATFORM
    uint64_t portable=quota_portable_service_next_deadline_ms(sleeping);if(portable<deadline)deadline=portable;
#endif
    if(pairing_requested())deadline=now+20;
    if(deadline==UINT64_MAX)return portMAX_DELAY;
    if(deadline<=now)return 0;
    TickType_t ticks=pdMS_TO_TICKS(deadline-now);return ticks?ticks:1;
}
static void network_task(void *arg)
{
    (void)arg;
    for(;;) {
        display_state_t waiting=display_state_snapshot();
        (void)ulTaskNotifyTake(pdTRUE,network_wait(waiting.sleeping));
        display_state_t display=display_state_snapshot();
        service_pairing_tick(display.sleeping);
#ifdef ESP_PLATFORM
        quota_portable_service_tick(display.sleeping,display.generation);
#endif
        if(display.sleeping)stop_wifi_for_sleep();
        service_pairing_tick(display.sleeping);
        mutex_lock();s_view.pairing_active=pairing_active_locked(monotonic_ms());
        s_view.pairing_seconds_left=s_view.pairing_active?(uint32_t)((QUOTA_PAIRING_WINDOW_MS-(monotonic_ms()-(uint64_t)s_pairing_opened_at_ms)+999)/1000):0;mutex_unlock();
    }
}

static void release_usb_decoder(void)
{
    if(s_usb_decoder){quota_portable_clear_secret(s_usb_decoder,sizeof(*s_usb_decoder));free(s_usb_decoder);s_usb_decoder=NULL;}
    s_usb_partial_at=0;
}
static bool usb_authorized(const char *session)
{
    if(!usb_active()||!s_usb_opener[0]||!session||strlen(session)!=QUOTA_USB_SESSION_BYTES)return false;
    unsigned difference=0;for(unsigned i=0;i<QUOTA_USB_SESSION_BYTES;i++)difference|=(unsigned char)session[i]^(unsigned char)s_usb_session[i];
    return difference==0;
}
static void send_usb_result(const char *id,const char *session,const char *error,bool accepted)
{
    if(!error&&(!usb_active()||(session&&!usb_authorized(session))))error="session_expired";
    char response[320];
    int length=error?snprintf(response,sizeof(response),"@AIQ:{\"v\":2,\"op\":\"result\",\"request_id\":\"%.8s\",\"ok\":false,\"error_code\":\"%s\"}\n",id?id:"00000000",error):
        snprintf(response,sizeof(response),"@AIQ:{\"v\":2,\"op\":\"result\",\"request_id\":\"%.8s\",\"ok\":true,\"session_id\":\"%.32s\",\"accepted\":%s}\n",id?id:"00000000",session?session:"",accepted?"true":"false");
    if(length>0&&(size_t)length<sizeof(response))(void)fwrite(response,1,(size_t)length,stdout);
    (void)fflush(stdout);
}
static void send_usb_state(const char *id,const char *session)
{
    char *response=malloc(QUOTA_USB_RESPONSE_BYTES);
    if(!response){send_usb_result(id,session,"no_memory",false);return;}
    int prefix=snprintf(response,QUOTA_USB_RESPONSE_BYTES,"@AIQ:{\"v\":2,\"op\":\"state\",\"request_id\":\"%.8s\",\"session_id\":\"%.32s\",\"ok\":true,\"state\":",id,session);
    size_t length=0;bool ok=prefix>0&&(size_t)prefix+QUOTA_PORTABLE_STATE_BYTES+3<=QUOTA_USB_RESPONSE_BYTES;
#ifdef ESP_PLATFORM
    if(ok)ok=quota_portable_service_state_json(response+prefix,QUOTA_PORTABLE_STATE_BYTES+1,&length,QUOTA_SETUP_USB)&&length&&length<=QUOTA_PORTABLE_STATE_BYTES;
#else
    ok=false;
#endif
    if(ok&&usb_authorized(session)){response[prefix+length]='}';response[prefix+length+1]='\n';(void)fwrite(response,1,(size_t)prefix+length+2,stdout);(void)fflush(stdout);}
    else send_usb_result(id,NULL,usb_authorized(session)?"state_unavailable":"session_expired",false);
    quota_portable_clear_secret(response,QUOTA_USB_RESPONSE_BYTES);free(response);
}
static void new_usb_session(void)
{
    for(unsigned i=0;i<QUOTA_USB_SESSION_BYTES;i++)s_usb_session[i]="0123456789abcdef"[esp_random()&15];
    s_usb_session[QUOTA_USB_SESSION_BYTES]=0;
}
static void handle_serial_frame(const char *frame,size_t length)
{
    quota_usb_request_t *request=calloc(1,sizeof(*request));const char *error=NULL;
    bool valid=request&&quota_usb_parse(frame,length,request,&error);
    /* No borrowed frame survives processing or a state/HTTP allocation. */
    release_usb_decoder();
    if(!request){send_usb_result(NULL,NULL,"no_memory",false);return;}
    if(!valid){send_usb_result(request->request_id,NULL,error?error:"invalid_frame",false);goto done;}
    if(request->op==QUOTA_USB_OPEN){
        if(!usb_active())send_usb_result(request->request_id,NULL,"session_expired",false);
        /* One serial port has one host owner, so an opener silent past the idle limit has lost its link (page reload) and may be replaced. */
        else if(s_usb_opener[0]&&strcmp(s_usb_opener,request->request_id)&&monotonic_ms()-s_usb_opener_at<QUOTA_USB_OPENER_IDLE_MS)send_usb_result(request->request_id,NULL,"session_busy",false);
        else{
            /* A takeover revokes the previous page's session so only one page keeps control. */
            if(s_usb_opener[0]&&strcmp(s_usb_opener,request->request_id))new_usb_session();
            memcpy(s_usb_opener,request->request_id,9);s_usb_opener_at=monotonic_ms();
            char response[320];uint64_t left=(usb_deadline_ms()-monotonic_ms()+999)/1000;
            int bytes=snprintf(response,sizeof(response),"@AIQ:{\"v\":2,\"op\":\"result\",\"request_id\":\"%.8s\",\"ok\":true,\"session_id\":\"%.32s\",\"remaining_seconds\":%u,\"max_command_bytes\":2048,\"max_frame_bytes\":4096,\"max_state_bytes\":16384}\n",request->request_id,s_usb_session,(unsigned)left);
            if(usb_active()&&bytes>0&&(size_t)bytes<sizeof(response))(void)fwrite(response,1,(size_t)bytes,stdout);
            (void)fflush(stdout);
        }
    }else if(!usb_authorized(request->session_id))send_usb_result(request->request_id,NULL,usb_active()?"invalid_session":"session_expired",false);
    else if(request->op==QUOTA_USB_STATE){
        s_usb_opener_at=monotonic_ms();
        char id[9],session[QUOTA_USB_SESSION_BYTES+1];memcpy(id,request->request_id,sizeof(id));memcpy(session,request->session_id,sizeof(session));
        quota_portable_clear_secret(request,sizeof(*request));free(request);request=NULL;
        send_usb_state(id,session);quota_portable_clear_secret(session,sizeof(session));
    }else{
        s_usb_opener_at=monotonic_ms();
        quota_portable_submit_result_t result=QUOTA_PORTABLE_SUBMIT_INVALID;
#ifdef ESP_PLATFORM
        if(request->op==QUOTA_USB_COMMAND)result=quota_portable_service_submit(&request->command,QUOTA_SETUP_USB);
#endif
        static const char *errors[]={NULL,"busy","session_expired","invalid_command","request_conflict"};
        send_usb_result(request->request_id,request->session_id,(unsigned)result<sizeof(errors)/sizeof(errors[0])?errors[result]:"invalid_command",result==QUOTA_PORTABLE_SUBMIT_ACCEPTED);
    }
done:
    if(request){quota_portable_clear_secret(request,sizeof(*request));free(request);}
}

/* Only this owner reads USB; idle sessions retain no decoder or TLS scratch. */
static void service_pairing_tick(bool sleeping)
{
    if(sleeping&&pairing_requested())quota_service_close_pairing_window();
    if(!pairing_requested()){
        release_usb_decoder();atomic_store(&s_usb_io_busy,false);atomic_store(&s_usb_deadline,0);
        quota_portable_clear_secret(s_usb_session,sizeof(s_usb_session));s_usb_opener[0]=0;return;
    }
    if(!usb_deadline_ms()){
        release_usb_decoder();atomic_store(&s_usb_io_busy,false);
#ifdef ESP_PLATFORM
        if(!quota_portable_service_prepare_usb())return;
#endif
        unsigned char ignored[64];unsigned drained=0;while(drained<4096){ssize_t count=read(STDIN_FILENO,ignored,sizeof(ignored));if(count<=0)break;drained+=(unsigned)count;}
        new_usb_session();s_usb_opener[0]=0;
        mutex_lock();s_pairing_screen_open=true;s_pairing_opened_at_ms=(int64_t)monotonic_ms();
        atomic_store(&s_usb_deadline,(uint64_t)s_pairing_opened_at_ms+QUOTA_PAIRING_WINDOW_MS);
        s_view.pairing_preparing=false;s_view.pairing_active=true;s_view.pairing_seconds_left=QUOTA_PAIRING_WINDOW_MS/1000;mutex_unlock();
        post_simple_event(QUOTA_APP_EVENT_PAIRING_TICK);
    }
    if(!usb_active()){quota_service_close_pairing_window();release_usb_decoder();atomic_store(&s_usb_io_busy,false);return;}
    if(s_usb_decoder&&monotonic_ms()-s_usb_partial_at>=3000){release_usb_decoder();atomic_store(&s_usb_io_busy,false);}
    for(unsigned i=0;i<512&&pairing_requested();i++){
        unsigned char input;if(read(STDIN_FILENO,&input,1)!=1)break;
        if(!usb_active()){quota_service_close_pairing_window();release_usb_decoder();atomic_store(&s_usb_io_busy,false);break;}
        if(!s_usb_decoder){s_usb_decoder=calloc(1,sizeof(*s_usb_decoder));if(!s_usb_decoder)break;quota_frame_decoder_init(s_usb_decoder);s_usb_partial_at=monotonic_ms();}
        atomic_store(&s_usb_io_busy,true);
        const char *frame=NULL;size_t length=0;
        quota_frame_result_t result=quota_frame_decoder_feed(s_usb_decoder,(char)input,&frame,&length);
        if(result==QUOTA_FRAME_COMPLETE){handle_serial_frame(frame,length);atomic_store(&s_usb_io_busy,false);}
        else if(result==QUOTA_FRAME_TOO_LONG){release_usb_decoder();atomic_store(&s_usb_io_busy,false);send_usb_result(NULL,NULL,"frame_too_long",false);}
        else if(input=='\n'){release_usb_decoder();atomic_store(&s_usb_io_busy,false);}
    }
    if(!pairing_requested()){release_usb_decoder();atomic_store(&s_usb_io_busy,false);}
}

bool quota_service_init(void)
{
    if(s_events)return true;
    s_mutex=xSemaphoreCreateMutex();if(!s_mutex)return false;
    s_events=xQueueCreate(EVENT_QUEUE_DEPTH,sizeof(quota_app_event_t));if(!s_events)return false;
    esp_err_t err=nvs_flash_init();s_nvs_ready=err==ESP_OK;
    if(!s_nvs_ready)ESP_LOGE(TAG,"NVS init failed (%s); stored credentials are unavailable",esp_err_to_name(err));
    (void)quota_store_init();
    if(!quota_store_erase_retired(s_nvs_ready))ESP_LOGW(TAG,"retired storage cleanup incomplete; it repeats at next boot");
    s_config_generation=1;
    s_view.screen_timeout_seconds=QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
    s_view.refresh_seconds=QUOTA_REFRESH_DEFAULT_SECONDS;s_view.auto_refresh=true;
#ifdef ESP_PLATFORM
    quota_portable_service_hooks_t hooks={.view=&s_view,.lock=mutex_lock,.unlock=mutex_unlock,.try_lock=portable_try_lock,
        .config_generation_locked=portable_config_generation_locked,.config_changed_locked=portable_config_changed_locked,
        .pairing_requested=pairing_requested,.usb_blocked=usb_blocked,.usb_active=usb_active,.usb_deadline_ms=usb_deadline_ms,.usb_close=quota_service_close_pairing_window,.wifi_ready=init_wifi,.wifi_stop=portable_wifi_stop,
        .notify=portable_notify,.wake=portable_wake,.display_current=display_generation_is_current};
    if(!quota_portable_service_init(&hooks))return false;
#endif
    s_pairing_screen_open=false;s_pairing_opened_at_ms=(int64_t)monotonic_ms();return true;
}

bool quota_service_start(void)
{
    if (s_events == NULL || s_mutex == NULL) return false;
    /* The basic USB console polls the FIFO without waiting. In IDF 5.5.3,
     * O_NONBLOCK instead checks a driver ring buffer that we do not install. */
    usb_serial_jtag_vfs_use_nonblocking();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0) return false;
    if (s_network_task == NULL && xTaskCreate(network_task, "quota_network",
            NETWORK_TASK_STACK, NULL, NETWORK_TASK_PRIORITY, &s_network_task) != pdPASS) {
        s_network_task = NULL;
        return false;
    }
    return true;
}

QueueHandle_t quota_service_event_queue(void)
{
    return s_events;
}

void quota_service_send_button(bsp_btn_t button, bsp_btn_ev_t event)
{
    quota_app_event_t app_event = {
        .kind = QUOTA_APP_EVENT_BUTTON,
        .button = button,
        .button_event = event,
    };
    post_event(&app_event, 0);
}

void quota_service_set_display_sleeping(bool sleeping)
{
    bool changed = false;
    portENTER_CRITICAL(&s_display_state_mux);
    if (s_display_scheduler.sleeping != sleeping) {
        s_display_scheduler.sleeping = sleeping;
        s_display_scheduler.generation++;
        if (s_display_scheduler.generation == 0) s_display_scheduler.generation = 1;
        changed = true;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    if (!changed) return;
    if (sleeping) quota_service_close_pairing_window();

    if (sleeping && s_mutex != NULL && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        s_view.refreshing = false;
        (void)xSemaphoreGive(s_mutex);
    }
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

void quota_service_get_view(quota_service_view_t *view)
{
    if (view == NULL) return;
    mutex_lock();
    *view = s_view;
    display_state_t display_state = display_state_snapshot();
    if (display_state.sleeping) {
        view->refreshing = false;
    }
    view->now_epoch = current_epoch();
    view->pairing_active = pairing_active_locked(monotonic_ms());
    if (view->pairing_active) {
        uint64_t elapsed = monotonic_ms() - (uint64_t)s_pairing_opened_at_ms;
        view->pairing_seconds_left = (uint32_t)((QUOTA_PAIRING_WINDOW_MS - elapsed + 999) / 1000);
    } else {
        view->pairing_seconds_left = 0;
    }
#ifdef ESP_PLATFORM
    quota_portable_service_countdown_overlay_locked(view);
#endif
    mutex_unlock();
}

void quota_service_get_selected_account_id(char account_id[QUOTA_ACCOUNT_ID_BYTES+1])
{
    if(!account_id)return;
    account_id[0]=0;
#ifdef ESP_PLATFORM
    (void)quota_portable_service_selected(account_id);
#endif
}

void quota_service_request_refresh(void) { quota_portable_service_refresh(); }

void quota_service_request_settings(uint16_t refresh_seconds,bool auto_refresh,uint16_t screen_timeout_seconds)
{
    quota_portable_service_settings(refresh_seconds,auto_refresh,screen_timeout_seconds);
}

void quota_service_select_account(const char *account_id) { quota_portable_service_select(account_id); }

void quota_service_open_pairing_window(void)
{
    atomic_store(&s_usb_deadline, 0);
    atomic_store(&s_pairing_requested, true);
    mutex_lock();
    s_pairing_screen_open = false;
    s_view.pairing_preparing = true; s_view.pairing_active = false;
    s_view.pairing_seconds_left = 0;
    mutex_unlock();
    if (s_network_task) xTaskNotifyGive(s_network_task);
    post_simple_event(QUOTA_APP_EVENT_PAIRING_TICK);
}

void quota_service_close_pairing_window(void)
{
    atomic_store(&s_pairing_requested, false);
    atomic_store(&s_usb_deadline, 0);
    mutex_lock();
    s_pairing_screen_open = false;
    s_view.pairing_preparing = s_view.pairing_active = false;
    s_view.pairing_seconds_left = 0;
    mutex_unlock();
    if (s_network_task) xTaskNotifyGive(s_network_task);
}

void quota_service_open_phone(void) { quota_portable_service_open(); }
void quota_service_close_phone(void) { quota_portable_service_close(); }
void quota_service_renew_phone(void) { quota_portable_service_renew(); }
void quota_service_cancel_auth(void) { quota_portable_service_cancel_auth(); }
void quota_service_reconnect(void) { quota_portable_service_reconnect(); }

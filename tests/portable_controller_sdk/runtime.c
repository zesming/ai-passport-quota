#include "quota_portable_service.h"
#include "quota_direct.h"
#include "quota_store.h"
#include "quota_portal.h"
#include "sdk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now_ms=100, wall=1800000000;
static bool common_locked,pairing,connected,network_ok=true,store_fail,cache_fail,acquire_fail;
static unsigned config_generation=1,query_calls,refresh_calls,retry_calls,release_calls;
static quota_service_view_t public_view;
static quota_direct_credential_t credentials[QUOTA_MAX_ACCOUNTS];
static bool used[QUOTA_MAX_ACCOUNTS];
static quota_snapshot_t saved_cache;
static quota_direct_credential_t *acquired;
static quota_portal_callbacks_t portal;
static bool portal_active,portal_fail,sntp_fail;static unsigned portal_starts;static esp_sntp_config_t sntp_config;
static void unlocked(void){assert(!common_locked);}
static int fake_gettimeofday(struct timeval *out,void *zone){(void)zone;out->tv_sec=(time_t)wall;out->tv_usec=0;return 0;}
static int fake_settimeofday(const struct timeval *in,const struct timezone *zone){(void)zone;wall=(uint64_t)in->tv_sec;return 0;}
/* The public snapshot is shared with the UI: observation merges into it must hold the common lock. */
static void checked_copy_observation(quota_snapshot_t *target,size_t target_index,const quota_snapshot_t *source,size_t source_index){if(target==&public_view.snapshot)assert(common_locked);quota_catalog_copy_observation(target,target_index,source,source_index);}
#define quota_catalog_copy_observation checked_copy_observation
#define gettimeofday fake_gettimeofday
#define settimeofday fake_settimeofday
#include "quota_portable_service.c"
#undef quota_catalog_copy_observation
#undef gettimeofday
#undef settimeofday
struct quota_direct{quota_direct_hooks_t hooks;quota_direct_credential_t *pending;bool active;};
static struct quota_direct provider;
static quota_direct_result_code_t query_code=QUOTA_DIRECT_OK,refresh_code=QUOTA_DIRECT_OK,login_code=QUOTA_DIRECT_WAITING;
int64_t esp_timer_get_time(void){return (int64_t)now_ms*1000;}
uint32_t esp_random(void){static uint32_t n=7;return n++;}
size_t esp_get_free_heap_size(void){return 60000;}
size_t esp_get_minimum_free_heap_size(void){return 1000;}
size_t heap_caps_get_largest_free_block(int caps){(void)caps;return 30000;}
UBaseType_t uxTaskGetStackHighWaterMark(void *task){(void)task;return 1024;}
static esp_netif_t netif;
esp_netif_t *esp_netif_create_default_wifi_ap(void){unlocked();return &netif;}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key){(void)key;unlocked();return &netif;}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *out){(void)n;unlocked();out->ip.addr=connected?1:0;return ESP_OK;}
esp_err_t esp_wifi_set_mode(int mode){(void)mode;unlocked();return ESP_OK;}
static wifi_config_t wifi_config;
esp_err_t esp_wifi_set_config(int iface,const wifi_config_t *value){unlocked();if(iface==WIFI_IF_STA)wifi_config=*value;return ESP_OK;}
esp_err_t esp_wifi_connect(void){unlocked();connected=network_ok;return ESP_OK;}
esp_err_t esp_wifi_disconnect(void){unlocked();connected=false;return ESP_OK;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out){unlocked();if(!connected)return ESP_FAIL;memset(out,0,sizeof(*out));memcpy(out->ssid,wifi_config.sta.ssid,32);return ESP_OK;}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config){unlocked();sntp_config=*config;return ESP_OK;}
esp_err_t esp_netif_sntp_sync_wait(unsigned timeout){(void)timeout;unlocked();return sntp_fail?ESP_FAIL:ESP_OK;}
void esp_netif_sntp_deinit(void){unlocked();}
bool quota_portal_start(const char *secret,const quota_portal_callbacks_t *callbacks){unlocked();portal_starts++;if(portal_fail)return false;assert(strlen(secret)==43);portal=*callbacks;portal_active=true;return true;}
void quota_portal_stop(void){unlocked();portal_active=false;}
bool quota_store_init(void){unlocked();return true;}
quota_portable_credential_t *quota_store_credential_acquire(void){unlocked();if(acquired||acquire_fail)return NULL;acquired=calloc(1,sizeof(*acquired));return acquired;}
void quota_store_credential_release(quota_portable_credential_t *value){unlocked();assert(value==acquired);assert(provider.pending!=value);quota_portable_clear_secret(value,sizeof(*value));free(value);acquired=NULL;release_calls++;}
static quota_store_read_result_t credential_read_error=QUOTA_STORE_READ_OK;
static quota_store_read_result_t orphan_error=QUOTA_STORE_READ_OK;static unsigned orphan_releases;
quota_store_read_result_t quota_store_release_orphan_credentials(void){unlocked();orphan_releases++;if(orphan_error!=QUOTA_STORE_READ_OK)return orphan_error;for(unsigned i=0;i<QUOTA_MAX_ACCOUNTS;i++)if(used[i]&&!credentials[i].tombstone){credentials[i].tombstone=true;credentials[i].access_token[0]=credentials[i].refresh_token[0]=credentials[i].api_key[0]=0;}return QUOTA_STORE_READ_OK;}
quota_store_read_result_t quota_store_load_credential_result(uint8_t slot,quota_portable_credential_t *out){unlocked();assert(out==acquired);memset(out,0,sizeof(*out));if(credential_read_error!=QUOTA_STORE_READ_OK)return credential_read_error;if(!used[slot])return QUOTA_STORE_READ_MISSING;*out=credentials[slot];return QUOTA_STORE_READ_OK;}
bool quota_store_save_credential(uint8_t slot,const quota_portable_credential_t *in){unlocked();assert(in==acquired);if(store_fail)return false;credentials[slot]=*in;used[slot]=true;return true;}
bool quota_store_remove_credential(uint8_t slot,const char *id,uint32_t gen){unlocked();assert(!acquired);if(store_fail)return false;memset(&credentials[slot],0,sizeof(credentials[slot]));strcpy(credentials[slot].id,id);credentials[slot].slot=slot;credentials[slot].generation=gen;credentials[slot].provider=QUOTA_PROVIDER_CODEX;credentials[slot].tombstone=true;used[slot]=true;return true;}
quota_direct_t *quota_direct_create(const quota_direct_hooks_t *hooks,quota_direct_transport_t transport,void *context){unlocked();(void)transport;(void)context;provider.hooks=*hooks;return &provider;}
bool quota_direct_has_pending_persist(const quota_direct_t *direct){return direct&&direct->pending;}
void quota_direct_login_cancel(quota_direct_t *direct){if(direct)direct->active=false;}
static quota_direct_result_t answer(quota_direct_result_code_t code,const quota_direct_credential_t *c){quota_direct_result_t r={0};r.code=code;r.http_status=200;if(code==QUOTA_DIRECT_OK){r.source_valid=true;r.source_epoch=wall;strcpy(r.account.id,c->id);r.account.provider=c->provider;r.account.has_observed_at=true;r.account.observed_at=wall;r.account.status=QUOTA_STATUS_OK;}if(code==QUOTA_DIRECT_RATE_LIMITED)r.retry_after_seconds=90;return r;}
quota_direct_result_t quota_direct_login_begin(quota_direct_t *direct,const quota_direct_credential_t *c,uint64_t time,uint64_t epoch){unlocked();(void)time;(void)epoch;assert(direct->hooks.admit(NULL,c->id,c->generation));direct->active=true;return answer(QUOTA_DIRECT_WAITING,c);}
quota_direct_result_t quota_direct_login_step(quota_direct_t *direct,quota_direct_credential_t *c,uint64_t time,uint64_t epoch){unlocked();(void)time;(void)epoch;if(login_code==QUOTA_DIRECT_PERSIST_PENDING){strcpy(c->access_token,"received-login-token");c->auth_state=QUOTA_PORTABLE_AUTH_READY;direct->pending=c;}return answer(login_code,c);}
void quota_direct_login_view(const quota_direct_t *direct,uint64_t time,quota_direct_login_view_t *out){(void)time;memset(out,0,sizeof(*out));out->active=direct->active;strcpy(out->verification_url,"https://auth.openai.com/codex/device");strcpy(out->user_code,"fake-code");}
quota_direct_result_t quota_direct_refresh(quota_direct_t *direct,quota_direct_credential_t *c,uint64_t epoch){unlocked();(void)epoch;refresh_calls++;assert(direct->hooks.admit(NULL,c->id,c->generation));if(refresh_code==QUOTA_DIRECT_PERSIST_PENDING){strcpy(c->refresh_token,"received-rotation");direct->pending=c;}return answer(refresh_code,c);}
quota_direct_result_t quota_direct_query(quota_direct_t *direct,quota_direct_credential_t *c,uint64_t epoch){unlocked();(void)epoch;if(!direct->hooks.admit(NULL,c->id,c->generation))return answer(QUOTA_DIRECT_DEFERRED,c);query_calls++;return answer(query_code,c);}
quota_direct_result_t quota_direct_retry_persist(quota_direct_t *direct){unlocked();retry_calls++;assert(direct->pending==acquired);if(!direct->hooks.persist(NULL,direct->pending))return answer(QUOTA_DIRECT_PERSIST_PENDING,direct->pending);quota_direct_result_t out=answer(QUOTA_DIRECT_OK,direct->pending);direct->pending=NULL;return out;}
static void common_lock(void){assert(!common_locked);common_locked=true;}
static void common_unlock(void){assert(common_locked);common_locked=false;}
static bool common_try(void){if(common_locked)return false;common_lock();return true;}
static uint32_t generation_locked(void){assert(common_locked);return config_generation;}
static void changed_locked(void){assert(common_locked);config_generation++;}
static bool pairing_requested(void){return pairing;}
static bool usb_enabled,usb_window,usb_scratch;
static uint64_t usb_deadline;
static unsigned usb_closes;
static bool hook_usb_blocked(void){return usb_enabled?usb_scratch:pairing;}
static bool hook_usb_active(void){return usb_enabled&&usb_window&&now_ms<usb_deadline;}
static uint64_t hook_usb_deadline(void){return usb_deadline;}
static void hook_usb_close(void){usb_window=false;usb_closes++;}
static void usb_open(void){usb_enabled=true;usb_window=true;usb_deadline=now_ms+120000;assert(quota_portable_service_prepare_usb());}
static bool wifi_ready(void){unlocked();return true;}
static bool wifi_stop(void){unlocked();connected=false;return true;}
static void notify(void){unlocked();}
static void hook_wake(void){unlocked();}
static bool display_current(uint32_t generation){(void)generation;return !s_sleeping;}

static quota_model_t durable_model;
static uint64_t durable_sequence;
static quota_store_read_result_t model_read_error=QUOTA_STORE_READ_OK;
static bool model_fail,model_unknown,model_apply_unknown;
quota_store_read_result_t quota_store_load_model_result(quota_model_t *out,uint64_t *sequence){unlocked();if(model_read_error!=QUOTA_STORE_READ_OK)return model_read_error;if(!durable_sequence)return QUOTA_STORE_READ_MISSING;*out=durable_model;*sequence=durable_sequence;return QUOTA_STORE_READ_OK;}
quota_model_write_result_t quota_store_save_model_verified(uint64_t previous,const quota_model_t *candidate,uint64_t sequence){unlocked();if(!quota_catalog_valid(candidate))fprintf(stderr,"invalid model networks=%u selected=%u refresh=%u timeout=%u entries=%u intent=%u\n",candidate->network_count,candidate->selected_network,candidate->refresh_seconds,candidate->screen_timeout_seconds,candidate->entry_count,candidate->intent.kind);assert(quota_catalog_valid(candidate));if(durable_sequence==sequence&&!memcmp(candidate,&durable_model,sizeof(*candidate)))return QUOTA_MODEL_APPLIED;if(model_unknown){if(model_apply_unknown){durable_model=*candidate;durable_sequence=sequence;}return QUOTA_MODEL_WRITE_UNKNOWN;}if(model_fail||store_fail)return QUOTA_MODEL_NOT_APPLIED;assert(durable_sequence==previous);durable_model=*candidate;durable_sequence=sequence;return QUOTA_MODEL_APPLIED;}
bool quota_store_save_observations(const quota_model_t *model,const quota_snapshot_t *snapshot,uint64_t time){unlocked();(void)model;(void)time;if(cache_fail)return false;saved_cache=*snapshot;return true;}
quota_store_read_result_t quota_store_load_observations(const quota_model_t *model,uint64_t time,quota_snapshot_t *snapshot){unlocked();(void)model;(void)time;for(unsigned i=0;i<saved_cache.account_count;i++){int index=quota_find_account_by_id(snapshot,saved_cache.accounts[i].id);if(index>=0)quota_catalog_copy_observation(snapshot,(size_t)index,&saved_cache,i);}return QUOTA_STORE_READ_OK;}
static void initialize(void){for(unsigned i=0;i<2;i++){used[i]=true;credentials[i].slot=i;credentials[i].generation=1;credentials[i].provider=i?QUOTA_PROVIDER_DEEPSEEK:QUOTA_PROVIDER_CODEX;credentials[i].auth_state=QUOTA_PORTABLE_AUTH_READY;snprintf(credentials[i].id,sizeof(credentials[i].id),"%032x",i+1);strcpy(credentials[i].server_account_id,"fake-server");strcpy(credentials[i].access_token,"old-access");strcpy(credentials[i].refresh_token,"old-refresh");strcpy(credentials[i].api_key,"old-key");credentials[i].expires_at=wall+3600;}saved_cache.account_count=1;saved_cache.refresh_seconds=300;saved_cache.accounts[0].provider=QUOTA_PROVIDER_CODEX;strcpy(saved_cache.accounts[0].id,credentials[0].id);saved_cache.accounts[0].has_observed_at=true;saved_cache.accounts[0].observed_at=wall;saved_cache.accounts[0].five_hour.present=true;saved_cache.accounts[0].five_hour.remaining_percent=17;}
static bool fresh_device;
static void seed_model(void);
static void boot(void){if(!durable_sequence&&!fresh_device)seed_model();quota_portable_service_hooks_t hooks={.view=&public_view,.lock=common_lock,.unlock=common_unlock,.try_lock=common_try,.config_generation_locked=generation_locked,.config_changed_locked=changed_locked,.pairing_requested=pairing_requested,.usb_blocked=hook_usb_blocked,.usb_active=hook_usb_active,.usb_deadline_ms=hook_usb_deadline,.usb_close=hook_usb_close,.wifi_ready=wifi_ready,.wifi_stop=wifi_stop,.notify=notify,.wake=hook_wake,.display_current=display_current};assert(quota_portable_service_init(&hooks));}
static void tick(bool sleeping){quota_portable_service_tick(sleeping,1);assert(!common_locked);}
static void ready(void){tick(false);now_ms+=500;tick(false);assert(public_view.portable.network_state==QUOTA_PORTABLE_NETWORK_READY);}
static void phone(void){quota_portable_service_open();tick(false);assert(portal_active&&public_view.portable.setup_ready);}
static void submit_command(quota_portable_command_t *command){command->phone_utc=wall;assert(portal.submit(command,NULL)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);tick(false);}
static job_t *job(const char *id){for(unsigned i=0;i<s_job_count;i++)if(!strcmp(s_jobs[i].id,id))return &s_jobs[i];assert(false);return NULL;}
static void close_phone(void){quota_portable_service_close();tick(false);now_ms+=500;ready();}
static void seed_model(void){model_defaults(&durable_model);durable_model.network_count=1;strcpy(durable_model.networks[0].ssid,"old-hotspot");strcpy(durable_model.networks[0].password,"password");for(unsigned i=0;i<QUOTA_MAX_ACCOUNTS;i++){if(!used[i])continue;quota_catalog_entry_t *e=&durable_model.entries[durable_model.entry_count++];strcpy(e->logical_id,credentials[i].id);e->provider=credentials[i].provider;e->source=QUOTA_ACCOUNT_DEVICE;e->activity=QUOTA_ACCOUNT_ACTIVE;e->row_generation=1;e->binding.native.slot=i;strcpy(e->binding.native.credential_id,credentials[i].id);e->binding.native.credential_generation=credentials[i].generation;}durable_sequence=1;}
static void seed_intent(void){seed_model();quota_model_intent_t *intent=&durable_model.intent;intent->kind=QUOTA_INTENT_UPSERT_NATIVE;intent->provider=QUOTA_PROVIDER_DEEPSEEK;intent->slot=1;strcpy(intent->logical_id,credentials[1].id);intent->expected_row_generation=1;strcpy(intent->previous_credential_id,credentials[1].id);intent->previous_credential_generation=1;strcpy(intent->target_credential_id,credentials[1].id);intent->target_credential_generation=2;strcpy(intent->desired_label,"recovered alias");}
static void all_native(void){for(unsigned i=2;i<8;i++){credentials[i]=credentials[1];credentials[i].slot=i;snprintf(credentials[i].id,sizeof(credentials[i].id),"%032x",i+1);used[i]=true;}}
int main(int argc,char **argv){assert(argc==2);initialize();
if(!strcmp(argv[1],"boot")){credentials[0].refresh_inflight=true;acquire_fail=true;boot();assert(!public_view.snapshot_valid);acquire_fail=false;now_ms+=5000;tick(false);assert(public_view.snapshot.account_count==2&&public_view.snapshot.accounts[0].five_hour.remaining_percent==17);assert(public_view.snapshot.accounts[0].status==QUOTA_STATUS_EXPIRED);assert(!public_view.snapshot.accounts[1].has_observed_at);assert(!acquired);}
else if(!strcmp(argv[1],"pending")){credentials[0].expires_at=1;refresh_code=QUOTA_DIRECT_PERSIST_PENDING;boot();ready();quota_portable_service_refresh();tick(false);assert(acquired&&provider.pending==acquired&&s_operation.kind==OP_SAVE);uint64_t due=s_operation.retry_at;assert(!quota_portable_service_prepare_usb());unsigned http=refresh_calls+query_calls;store_fail=true;now_ms=due;tick(true);assert(retry_calls==1&&acquired);now_ms+=5000;tick(true);assert(retry_calls==2);store_fail=false;now_ms+=30000;tick(true);assert(!provider.pending&&!acquired&&s_operation.kind==OP_NONE);assert(refresh_calls+query_calls==http);assert(!strcmp(credentials[0].refresh_token,"received-rotation"));assert(quota_portable_service_next_deadline_ms(true)==UINT64_MAX);}
else if(!strcmp(argv[1],"key")){boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);strcpy(cmd.api_key,"candidate-key");strcpy(cmd.label,"candidate");submit_command(&cmd);assert(acquired&&!strcmp(credentials[1].api_key,"old-key"));assert(query_calls==0);close_phone();assert(!acquired&&!strcmp(credentials[1].api_key,"candidate-key"));assert(credentials[1].generation==2);assert(durable_model.intent.kind==QUOTA_INTENT_NONE);assert(!strcmp(durable_model.entries[1].label,"candidate"));assert(job(cmd.request_id)->state==2);}
else if(!strcmp(argv[1],"cancel")){boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.api_key,"candidate-key");submit_command(&cmd);assert(public_view.snapshot.account_count==2&&durable_model.intent.kind==QUOTA_INTENT_UPSERT_NATIVE);quota_portable_command_t cancel={.op=QUOTA_PORTABLE_OP_OPERATION_CANCEL};strcpy(cancel.request_id,"22345678");strcpy(cancel.target_request_id,cmd.request_id);submit_command(&cancel);assert(!acquired&&public_view.snapshot.account_count==2);assert(durable_model.intent.kind==QUOTA_INTENT_NONE);assert(!strcmp(job(cmd.request_id)->error,"canceled"));}
else if(!strcmp(argv[1],"queue")){boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_NETWORK_SAVE,.network_index=0};strcpy(cmd.request_id,"12345678");strcpy(cmd.ssid,"new-hotspot");strcpy(cmd.password,"password");assert(portal.submit(&cmd,NULL)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);assert(portal.submit(&cmd,NULL)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);common_lock();config_generation++;common_unlock();tick(false);assert(!s_candidate_pending&&!strcmp(job(cmd.request_id)->error,"configuration_changed"));strcpy(cmd.request_id,"22345678");submit_command(&cmd);network_ok=false;quota_portable_service_close();tick(false);assert(s_candidate_pending);assert(public_view.portable.saved_network_count==1&&!strcmp(public_view.portable.saved_network_ssids[0],"old-hotspot"));now_ms+=25000;tick(false);assert(!s_candidate_pending&&!strcmp(durable_model.networks[0].ssid,"old-hotspot"));assert(job(cmd.request_id)->state==3);}
else if(!strcmp(argv[1],"cadence")){boot();ready();uint64_t deadline=s_next_refresh;tick(true);now_ms+=100;tick(false);assert(s_next_refresh==deadline);phone();assert(s_next_refresh==deadline);quota_portable_service_close();tick(false);assert(s_next_refresh==deadline);}
else if(!strcmp(argv[1],"state-contract")){boot();ready();quota_portable_service_refresh();for(unsigned i=0;i<5;i++){now_ms+=500;tick(false);}assert(query_calls>=2);assert(public_view.refresh_seconds==300);phone();quota_portable_command_t settings={.op=QUOTA_PORTABLE_OP_SETTINGS_SAVE,.refresh_seconds=900,.screen_timeout_seconds=120};strcpy(settings.request_id,"12345678");submit_command(&settings);assert(public_view.refresh_seconds==900&&job(settings.request_id)->state==2);char json[QUOTA_PORTABLE_STATE_BYTES];size_t length;assert(state_json(json,sizeof(json),&length,NULL));assert(!strstr(json,"pending_accounts")&&!strstr(json,"pending_network")&&!strstr(json,"\"source\"")&&!strstr(json,"source_changed"));assert(!strstr(json,"old-key")&&!strstr(json,"old-access")&&!strstr(json,"\"mode\""));}
else if(!strcmp(argv[1],"physical-gate")){boot();ready();assert(quota_portable_service_http_allowed());quota_portable_service_open();assert(!quota_portable_service_http_allowed());assert(!admit(NULL,credentials[0].id,1));tick(false);assert(portal_active);}
else if(!strcmp(argv[1],"typed-read")){credential_read_error=QUOTA_STORE_READ_IO_ERROR;boot();assert(!public_view.snapshot_valid&&durable_sequence==1);assert(!strcmp(s_storage_error,"storage_io_error"));assert(!quota_portable_service_http_allowed());credential_read_error=QUOTA_STORE_READ_OK;now_ms+=5000;tick(false);assert(public_view.snapshot.account_count==2);}
else if(!strcmp(argv[1],"unknown")){boot();phone();model_unknown=true;model_apply_unknown=true;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_SETTINGS_SAVE,.refresh_seconds=900,.screen_timeout_seconds=120};strcpy(cmd.request_id,"12345678");submit_command(&cmd);assert(s_dirty_model&&public_view.refresh_seconds==300&&job(cmd.request_id)->state==1);assert(!quota_portable_service_http_allowed());model_unknown=false;now_ms+=1000;tick(true);assert(!s_dirty_model&&public_view.refresh_seconds==900&&job(cmd.request_id)->state==2);}
else if(!strcmp(argv[1],"delete")){boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_ACCOUNT_REMOVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);submit_command(&cmd);tick(false);assert(public_view.snapshot.account_count==1&&credentials[1].tombstone&&credentials[1].generation==2);assert(durable_model.intent.kind==QUOTA_INTENT_NONE);}
else if(!strcmp(argv[1],"generation")){credentials[1].generation=UINT32_MAX;boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);strcpy(cmd.api_key,"candidate-key");submit_command(&cmd);assert(job(cmd.request_id)->state==3&&!strcmp(job(cmd.request_id)->error,"generation_exhausted"));assert(!acquired);}
else if(!strcmp(argv[1],"recovery-previous")){seed_intent();boot();assert(durable_model.intent.kind==QUOTA_INTENT_NONE&&durable_model.entries[1].binding.native.credential_generation==1);assert(!query_calls&&!refresh_calls&&!acquired);}
else if(!strcmp(argv[1],"recovery-target")){seed_intent();credentials[1].generation=2;strcpy(credentials[1].api_key,"durable-received-key");boot();assert(durable_model.intent.kind==QUOTA_INTENT_NONE&&durable_model.entries[1].binding.native.credential_generation==2&&durable_model.entries[1].row_generation==2);assert(!query_calls&&!refresh_calls&&!acquired);assert(!strcmp(durable_model.entries[1].label,"recovered alias"));}
else if(!strcmp(argv[1],"recovery-conflict")){seed_intent();used[1]=false;boot();assert(durable_model.intent.kind==QUOTA_INTENT_UPSERT_NATIVE&&!strcmp(s_storage_error,"recovery_conflict"));assert(!query_calls&&!refresh_calls&&!quota_portable_service_http_allowed());}
else if(!strcmp(argv[1],"invalid-model")){model_read_error=QUOTA_STORE_READ_INVALID;boot();assert(durable_sequence==1&&!public_view.snapshot_valid&&!strcmp(s_storage_error,"storage_invalid"));now_ms+=10000;tick(false);assert(durable_sequence==1&&!query_calls);}
else if(!strcmp(argv[1],"full-native")){all_native();boot();assert(public_view.snapshot.account_count==8);phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);strcpy(cmd.api_key,"candidate-key");submit_command(&cmd);assert(acquired);close_phone();assert(credentials[1].generation==2&&!acquired);}
else if(!strcmp(argv[1],"received-cancel")){boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);strcpy(cmd.api_key,"received-key");submit_command(&cmd);store_fail=true;close_phone();assert(acquired&&s_operation.kind==OP_SAVE&&s_operation.received);quota_portable_service_cancel_auth();quota_portable_service_open();pairing=true;tick(true);assert(acquired&&s_model.intent.kind==QUOTA_INTENT_UPSERT_NATIVE);unsigned http=query_calls+refresh_calls;store_fail=false;now_ms+=1000;tick(true);assert(!acquired&&!strcmp(credentials[1].api_key,"received-key")&&durable_model.intent.kind==QUOTA_INTENT_NONE);assert(query_calls+refresh_calls==http);}
else if(!strcmp(argv[1],"physical-login")){boot();phone();quota_portable_command_t queue={.op=QUOTA_PORTABLE_OP_CODEX_QUEUE};strcpy(queue.request_id,"12345678");submit_command(&queue);quota_portable_command_t launch={.op=QUOTA_PORTABLE_OP_CODEX_LAUNCH};strcpy(launch.request_id,"22345678");submit_command(&launch);close_phone();assert(s_operation.started);quota_portable_service_open();assert(!quota_portable_service_http_allowed());tick(false);assert(portal_active&&!acquired&&s_operation.kind==OP_NONE&&durable_model.intent.kind==QUOTA_INTENT_NONE);}
else if(!strcmp(argv[1],"received-model-unknown")){boot();phone();quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);strcpy(cmd.api_key,"received-key");submit_command(&cmd);model_unknown=true;model_apply_unknown=true;close_phone();assert(acquired&&s_dirty_model&&s_operation.kind==OP_SAVE);unsigned calls=query_calls;model_unknown=false;now_ms+=1000;tick(true);assert(!s_dirty_model&&!acquired&&s_model.intent.kind==QUOTA_INTENT_NONE&&credentials[1].generation==2);assert(query_calls==calls);}
else if(!strcmp(argv[1],"sleep-open-deadline")){boot();tick(true);quota_portable_service_open();assert(quota_portable_service_next_deadline_ms(true)==UINT64_MAX);tick(true);assert(s_open&&!portal_active&&quota_portable_service_next_deadline_ms(true)==UINT64_MAX);tick(false);assert(portal_active);}
else if(!strcmp(argv[1],"prepare-unknown")){boot();phone();model_unknown=true;quota_portable_command_t queue={.op=QUOTA_PORTABLE_OP_CODEX_QUEUE};strcpy(queue.request_id,"12345678");submit_command(&queue);assert(s_dirty_model&&job(queue.request_id)->state==1&&public_view.portable.login_state==QUOTA_PORTABLE_LOGIN_QUEUED);model_unknown=false;now_ms+=1000;tick(false);assert(!s_dirty_model&&job(queue.request_id)->state==2&&durable_model.intent.kind==QUOTA_INTENT_UPSERT_NATIVE);quota_portable_command_t launch={.op=QUOTA_PORTABLE_OP_CODEX_LAUNCH};strcpy(launch.request_id,"22345678");submit_command(&launch);assert(job(launch.request_id)->state==1&&public_view.portable.login_state==QUOTA_PORTABLE_LOGIN_CONNECTING);}
else if(!strcmp(argv[1],"prepare-unknown-cancel")){boot();phone();model_unknown=true;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.api_key,"candidate-key");submit_command(&cmd);quota_portable_command_t cancel={.op=QUOTA_PORTABLE_OP_OPERATION_CANCEL};strcpy(cancel.request_id,"22345678");strcpy(cancel.target_request_id,cmd.request_id);submit_command(&cancel);assert(s_operation.cancel_requested&&s_dirty_model&&acquired);model_unknown=false;now_ms+=1000;tick(false);assert(!s_dirty_model&&!acquired&&durable_model.intent.kind==QUOTA_INTENT_NONE&&job(cmd.request_id)->state==3&&!strcmp(job(cmd.request_id)->error,"canceled"));assert(!query_calls&&!refresh_calls);}
else if(!strcmp(argv[1],"unknown-corruption")){boot();phone();model_unknown=true;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_SETTINGS_SAVE,.refresh_seconds=900,.screen_timeout_seconds=120};strcpy(cmd.request_id,"12345678");submit_command(&cmd);model_read_error=QUOTA_STORE_READ_INVALID;now_ms+=1000;tick(true);assert(s_dirty_model&&s_recovery_blocked&&!strcmp(s_storage_error,"storage_invalid")&&public_view.refresh_seconds==300);assert(quota_portable_service_next_deadline_ms(true)==UINT64_MAX);}
else if(!strcmp(argv[1],"unknown-conflict")){boot();phone();model_unknown=true;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_SETTINGS_SAVE,.refresh_seconds=900,.screen_timeout_seconds=120};strcpy(cmd.request_id,"12345678");submit_command(&cmd);durable_sequence+=5;now_ms+=1000;tick(true);assert(s_dirty_model&&s_recovery_blocked&&!strcmp(s_storage_error,"recovery_conflict")&&public_view.refresh_seconds==300);assert(quota_portable_service_next_deadline_ms(true)==UINT64_MAX);}
else if(!strcmp(argv[1],"label-only-full")){all_native();credentials[1].generation=UINT32_MAX;boot();phone();uint32_t generation=durable_model.entries[1].row_generation;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.account_id,credentials[1].id);strcpy(cmd.label,"renamed alias");submit_command(&cmd);assert(job(cmd.request_id)->state==2&&!acquired&&credentials[1].generation==UINT32_MAX&&durable_model.entries[1].row_generation==generation&&!strcmp(durable_model.entries[1].label,"renamed alias"));assert(durable_model.intent.kind==QUOTA_INTENT_NONE&&!query_calls);}
else if(!strcmp(argv[1],"offline-deadlines")){boot();ready();s_model.auto_refresh=false;s_cache_at=now_ms-1;s_cache_dirty=true;s_clock_ready=false;s_view.network_state=QUOTA_PORTABLE_NETWORK_ERROR;unsigned calls=query_calls;assert(quota_portable_service_next_deadline_ms(false)>now_ms);source_tick();assert(query_calls==calls);}
else if(!strcmp(argv[1],"dirty-expired-deadline")){boot();phone();model_unknown=true;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(cmd.request_id,"12345678");strcpy(cmd.api_key,"candidate-key");submit_command(&cmd);now_ms+=QUOTA_PORTABLE_SETUP_MS+60001;tick(true);assert(s_dirty_model&&acquired&&quota_portable_service_next_deadline_ms(true)>now_ms);assert(!query_calls&&!refresh_calls);}
else if(!strcmp(argv[1],"v2-reauth-cache")){seed_model();credentials[0].refresh_inflight=true;boot();assert(public_view.snapshot.accounts[0].five_hour.remaining_percent==17&&public_view.snapshot.accounts[0].status==QUOTA_STATUS_EXPIRED&&!strcmp(public_view.portable.account_errors[0],"auth_required"));assert(!query_calls&&!refresh_calls);}
else if(!strcmp(argv[1],"unknown-valid-prior")){boot();phone();model_unknown=true;quota_portable_command_t cmd={.op=QUOTA_PORTABLE_OP_SETTINGS_SAVE,.refresh_seconds=900,.screen_timeout_seconds=120};strcpy(cmd.request_id,"12345678");submit_command(&cmd);quota_model_t *held=s_dirty_model;now_ms+=1000;tick(true);assert(s_dirty_model==held&&!s_recovery_blocked&&public_view.refresh_seconds==300);model_unknown=false;now_ms+=5000;tick(true);assert(!s_dirty_model&&public_view.refresh_seconds==900);}

else if(!strcmp(argv[1],"usb-key-network")){
    boot();ready();usb_open();assert(!portal_active&&quota_portable_service_http_allowed());
    quota_portable_command_t key={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE,.phone_utc=1800000000};
    strcpy(key.request_id,"12345678");strcpy(key.account_id,credentials[1].id);strcpy(key.api_key,"usb-new-key");strcpy(key.label,"USB account");
    assert(quota_portable_service_submit(&key,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);
    assert(quota_portable_service_submit(&key,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_ACCEPTED&&s_count==1);
    strcpy(key.api_key,"conflicting-key");assert(quota_portable_service_submit(&key,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_CONFLICT);
    tick(false);assert(!acquired&&!strcmp(credentials[1].api_key,"usb-new-key")&&job(key.request_id)->state==2&&usb_window&&!s_queue);
    quota_portable_command_t net={.op=QUOTA_PORTABLE_OP_NETWORK_SAVE,.network_index=0};strcpy(net.request_id,"22345678");strcpy(net.ssid,"USB network");strcpy(net.password,"password");
    assert(quota_portable_service_submit(&net,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);tick(false);
    assert(s_candidate_pending&&s_candidate_deadline==now_ms+25000);now_ms+=500;tick(false);
    assert(!s_candidate_pending&&!strcmp(durable_model.networks[0].ssid,"USB network")&&usb_window);
    usb_scratch=true;assert(!quota_portable_service_http_allowed());unsigned calls=query_calls;tick(false);assert(query_calls==calls);usb_scratch=false;
    quota_portable_command_t close={.op=QUOTA_PORTABLE_OP_SETUP_CLOSE};strcpy(close.request_id,"32345678");
    assert(quota_portable_service_submit(&close,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);tick(false);
    usb_window=false;now_ms++;usb_open();now_ms+=500;tick(false);assert(usb_window&&usb_closes==0); /* Old close cannot revoke a new window. */
    usb_window=false;assert(quota_portable_service_submit(&key,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_CLOSED);
}
else if(!strcmp(argv[1],"usb-login-reopen-save")){
    boot();phone();quota_portable_command_t queue={.op=QUOTA_PORTABLE_OP_CODEX_QUEUE};strcpy(queue.request_id,"12345678");submit_command(&queue);quota_direct_credential_t *held=acquired;
    usb_open();assert(!portal_active&&s_operation.kind==OP_LOGIN&&acquired==held&&s_view.login_state==QUOTA_PORTABLE_LOGIN_QUEUED&&job(queue.request_id)->state==2);
    quota_portable_command_t launch={.op=QUOTA_PORTABLE_OP_CODEX_LAUNCH};strcpy(launch.request_id,"22345678");
    assert(quota_portable_service_submit(&launch,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);tick(false);ready();assert(s_operation.started&&usb_window&&s_operation.deadline==s_login_deadline);
    strcpy(s_view.login_user_code,"TEST-CODE");s_view.login_state=QUOTA_PORTABLE_LOGIN_WAITING;char json[QUOTA_PORTABLE_STATE_BYTES+1];size_t length=0;
    assert(quota_portable_service_state_json(json,sizeof(json),&length,QUOTA_SETUP_USB));assert(strstr(json,"TEST-CODE")&&strstr(json,QUOTA_DIRECT_VERIFICATION_URL)&&!strstr(json,"old-refresh"));
    assert(quota_portable_service_state_json(json,sizeof(json),&length,QUOTA_SETUP_AP));assert(!strstr(json,"TEST-CODE")&&!strstr(json,"verification_url"));
    usb_window=false;usb_open();assert(s_operation.started&&acquired==held);
    quota_portable_command_t close={.op=QUOTA_PORTABLE_OP_SETUP_CLOSE};strcpy(close.request_id,"32345678");assert(quota_portable_service_submit(&close,QUOTA_SETUP_USB)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);tick(false);now_ms+=500;tick(false);
    assert(!usb_window&&usb_closes==1&&s_operation.started&&acquired==held);
    uint8_t login_slot=held->slot;usb_open();login_code=QUOTA_DIRECT_PERSIST_PENDING;store_fail=true;tick(false);assert(provider.pending==held&&s_operation.kind==OP_SAVE);
    usb_window=false;assert(!quota_portable_service_prepare_usb()&&provider.pending==held);
    store_fail=false;now_ms=s_operation.retry_at;tick(true);assert(!provider.pending&&!acquired&&!strcmp(credentials[login_slot].access_token,"received-login-token"));
    usb_open();assert(job(launch.request_id)->state==2&&s_operation.kind==OP_NONE);
}
else if(!strcmp(argv[1],"open-while-active")){
    /* A second open request is a no-op: it keeps the session, never spins, and never reopens a new hotspot after expiry. */
    boot();phone();char ssid[40];strcpy(ssid,public_view.portable.setup_ssid);unsigned starts=portal_starts;
    quota_portable_service_open();quota_portable_service_renew();assert(!s_open);assert(quota_portable_service_next_deadline_ms(false)>now_ms);
    s_open=true; /* a request latched before the session opened must also be consumed, not spun on */
    tick(false);assert(!s_open&&portal_starts==starts&&!strcmp(public_view.portable.setup_ssid,ssid)&&quota_portable_service_next_deadline_ms(false)>now_ms);
    now_ms+=QUOTA_PORTABLE_SETUP_MS;tick(false);assert(!public_view.portable.setup_active&&!s_open);
    now_ms+=500;tick(false);assert(!public_view.portable.setup_active&&portal_starts==starts&&quota_portable_service_next_deadline_ms(false)>now_ms);
}
else if(!strcmp(argv[1],"open-failure-backoff")){
    /* A failing hotspot start waits between bounded attempts instead of looping with a zero deadline. */
    boot();ready();portal_fail=true;quota_portable_service_open();tick(false);
    assert(portal_starts==1&&s_open&&!public_view.portable.setup_active);
    for(int i=0;i<1000;i++){if(quota_portable_service_next_deadline_ms(false)>now_ms)break;tick(false);}
    assert(portal_starts==1&&s_open_retry_at==now_ms+OPEN_RETRY_MS&&quota_portable_service_next_deadline_ms(false)>now_ms&&quota_portable_service_next_deadline_ms(false)<=s_open_retry_at);
    now_ms+=OPEN_RETRY_MS-1;tick(false);assert(portal_starts==1);
    now_ms+=1;tick(false);assert(portal_starts==2&&s_open);
    now_ms+=OPEN_RETRY_MS;tick(false);assert(portal_starts==3&&!s_open);
    assert(quota_portable_service_next_deadline_ms(false)>now_ms);now_ms+=60000;tick(false);assert(portal_starts==3);
    portal_fail=false;quota_portable_service_renew();tick(false);assert(portal_starts==4&&public_view.portable.setup_active&&!s_open);
}
else if(!strcmp(argv[1],"sntp-servers")){
    boot();ready();assert(s_sntp&&sntp_config.start&&sntp_config.num_of_servers==3&&sntp_config.num_of_servers<=CONFIG_LWIP_SNTP_MAX_SERVERS);
    assert(!strcmp(sntp_config.servers[0],"time.cloudflare.com")&&!strcmp(sntp_config.servers[1],"ntp.aliyun.com")&&!strcmp(sntp_config.servers[2],"pool.ntp.org"));
}
else if(!strcmp(argv[1],"usb-pauses-auto-refresh")){
    boot();ready();s_model.auto_refresh=true;s_next_refresh=now_ms;usb_open();
    unsigned calls=query_calls;tick(false);now_ms+=500;tick(false);assert(query_calls==calls&&!s_cycle);
    quota_portable_service_refresh();tick(false);assert(query_calls>calls); /* an explicit refresh still runs */
    for(int i=0;i<4&&s_cycle;i++){now_ms+=500;tick(false);}
    calls=query_calls;s_next_refresh=now_ms;tick(false);assert(query_calls==calls);
    usb_window=false;now_ms+=500;tick(false);assert(query_calls>calls); /* scheduled polling resumes once the window ends */
}
else if(!strcmp(argv[1],"fresh-device")){
    /* No catalog (new device, or one that only had retired data): start empty and open setup. */
    fresh_device=true;boot();assert(durable_sequence==1&&public_view.configured&&public_view.snapshot_valid);
    assert(public_view.snapshot.account_count==0&&durable_model.entry_count==0&&durable_model.network_count==0&&s_open);
    assert(orphan_releases==1&&credentials[0].tombstone&&credentials[1].tombstone&&!credentials[1].api_key[0]); /* Stale credentials are freed, never adopted. */
    tick(false);assert(portal_active&&public_view.portable.setup_ready&&!s_open);
}
else if(!strcmp(argv[1],"fresh-orphan-retry")){
    /* A failed release blocks the new catalog and is retried; nothing is committed meanwhile. */
    fresh_device=true;orphan_error=QUOTA_STORE_READ_IO_ERROR;boot();
    assert(!durable_sequence&&!public_view.snapshot_valid&&!strcmp(s_storage_error,"storage_io_error")&&!credentials[0].tombstone);
    orphan_error=QUOTA_STORE_READ_OK;now_ms+=5000;tick(false);
    assert(durable_sequence==1&&public_view.snapshot_valid&&credentials[0].tombstone&&orphan_releases==2);
}
else if(!strcmp(argv[1],"account-limit")){
    /* A full catalog refuses a ninth account instead of keeping a hidden row. */
    all_native();boot();assert(durable_model.entry_count==8&&public_view.snapshot.account_count==8);phone();
    quota_portable_command_t codex={.op=QUOTA_PORTABLE_OP_CODEX_QUEUE};strcpy(codex.request_id,"12345678");submit_command(&codex);
    assert(job(codex.request_id)->state==3&&!strcmp(job(codex.request_id)->error,"account_limit")&&!acquired&&durable_model.entry_count==8);
    quota_portable_command_t key={.op=QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};strcpy(key.request_id,"22345678");strcpy(key.api_key,"new-key");submit_command(&key);
    assert(job(key.request_id)->state==3&&!strcmp(job(key.request_id)->error,"account_limit")&&!acquired&&durable_model.entry_count==8);
}
else if(!strcmp(argv[1],"network-limit")){
    seed_model();durable_model.network_count=3;for(unsigned i=1;i<3;i++){snprintf(durable_model.networks[i].ssid,sizeof(durable_model.networks[i].ssid),"hotspot-%u",i);strcpy(durable_model.networks[i].password,"password");}
    boot();phone();quota_portable_command_t net={.op=QUOTA_PORTABLE_OP_NETWORK_SAVE,.network_index=UINT8_MAX};strcpy(net.request_id,"12345678");strcpy(net.ssid,"fourth-hotspot");strcpy(net.password,"password");submit_command(&net);
    assert(job(net.request_id)->state==3&&!strcmp(job(net.request_id)->error,"network_limit")&&!s_candidate_pending&&durable_model.network_count==3);
}
else assert(false);
puts("whole controller runtime passed");return 0;
}

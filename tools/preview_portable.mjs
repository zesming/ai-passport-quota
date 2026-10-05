// Synthetic, loopback-only UI fixture. Never talks to a device or provider.
// Run manually: node tools/preview_portable.mjs [--scenario=empty|offline|expired|pending|companion]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const htmlPath = path.join(root, 'main/portable_setup.html');
const port = 4328, secret = 'synthetic-preview-only';
let scenario = process.argv.find(item => item.startsWith('--scenario='))?.slice(11) || 'default';
const seedAccounts = () => [
  {id:'fixture-codex',provider:'codex',email:'preview@example.invalid',plan:'Plus',status:'ok',auth_state:'ready',observed_at:Math.floor(Date.now()/1000)-120,five_hour:{present:true,remaining_percent:68,resets_at:Math.floor(Date.now()/1000)+9000},seven_day:{present:true,remaining_percent:42,resets_at:Math.floor(Date.now()/1000)+320000}},
  {id:'fixture-deepseek',provider:'deepseek',label:'DeepSeek API',status:'ok',auth_state:'ready',observed_at:Math.floor(Date.now()/1000)-180,balance:{is_available:true,balance_infos:[{currency:'CNY',total_balance:'123.4567'}]}},
];
let state;
function reset(){state={mode:scenario==='companion'?'companion':'direct',session:{active:scenario!=='expired',remaining_seconds:scenario==='expired'?0:590},network:{state:scenario==='offline'?'error':'ap',connected:false,ssid:scenario==='empty'?'':'Preview Wi-Fi',ip:'',saved_networks:scenario==='empty'?[]:[{index:0,ssid:'Preview Wi-Fi',selected:true},{index:1,ssid:'Phone Hotspot',selected:false}]},clock:{synchronized:scenario!=='offline',epoch:Math.floor(Date.now()/1000)},settings:{auto_refresh:true,refresh_seconds:300,screen_timeout_seconds:120},accounts:scenario==='empty'?[]:seedAccounts(),jobs:[]};if(scenario==='pending')state.accounts[1]={...state.accounts[1],status:'waiting',auth_state:'pending',balance:null,observed_at:0};}
reset();
const idempotent = new Map();
function send(response,status,data){response.writeHead(status,{'Content-Type':'application/json;charset=utf-8','Cache-Control':'no-store'});response.end(JSON.stringify(data));}
function job(body,status='queued'){state.jobs.push({request_id:body.request_id,op:body.op,status});state.jobs=state.jobs.slice(-4);}
const server=http.createServer(async(request,response)=>{
 const url=new URL(request.url,'http://127.0.0.1:'+port);
 if(url.pathname.startsWith('/__preview/')){scenario=url.pathname.slice(11);reset();idempotent.clear();response.writeHead(302,{Location:'/#s='+secret});response.end();return;}
 if(url.pathname==='/'){response.writeHead(200,{'Content-Type':'text/html;charset=utf-8','Cache-Control':'no-store','Referrer-Policy':'no-referrer','Content-Security-Policy':"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"});response.end(fs.readFileSync(htmlPath));return;}
 if(request.headers['x-aiq-setup']!==secret)return send(response,401,{ok:false,error_code:'unauthorized'});
 if(!state.session.active)return send(response,401,{ok:false,error_code:'session_expired'});
 if(url.pathname==='/api/state'&&request.method==='GET')return send(response,200,state);
 if(url.pathname!=='/api/command'||request.method!=='POST')return send(response,404,{ok:false,error_code:'not_found'});
 let input='';for await(const chunk of request){input+=chunk;if(input.length>4096)return send(response,413,{ok:false,error_code:'invalid_request'});}
 let body;try{body=JSON.parse(input);}catch{return send(response,400,{ok:false,error_code:'invalid_request'});}
 if(body.v!==1||!/^[a-f0-9]{8}$/.test(body.request_id||''))return send(response,400,{ok:false,error_code:'invalid_request'});
 if(idempotent.has(body.request_id))return send(response,200,idempotent.get(body.request_id));
 switch(body.op){
  case 'network_save':{
   const saved=state.network.saved_networks;let index=body.network_index;
   if(body.ssid){if(index===undefined){index=saved.findIndex(item=>item.ssid===body.ssid);if(index<0)index=saved.length;}if(index>=3)return send(response,409,{ok:false,error_code:'network_limit'});const existing=saved.find(item=>item.index===index);if(existing)existing.ssid=body.ssid;else saved.push({index,ssid:body.ssid,selected:false});}
   saved.forEach(item=>item.selected=item.index===index);state.network.ssid=saved.find(item=>item.selected)?.ssid||'';job(body);break;
  }
  case 'deepseek_save':{let account=state.accounts.find(item=>item.id===body.account_id);if(account){account.label=body.label;if(body.api_key){account.auth_state='pending';account.status='waiting';}}else{account={id:'fixture-new-'+state.accounts.length,provider:'deepseek',label:body.label,status:'waiting',auth_state:'pending',observed_at:0};state.accounts.push(account);}job(body);break;}
  case 'account_remove':state.accounts=state.accounts.filter(item=>item.id!==body.account_id);job(body,'succeeded');break;
  case 'settings_save':state.settings={auto_refresh:body.auto_refresh,refresh_seconds:body.refresh_seconds,screen_timeout_seconds:body.screen_timeout_seconds};job(body,'succeeded');break;
  case 'mode_select':state.mode=body.mode;state.accounts=body.mode==='direct'&&scenario==='companion'?[]:state.accounts;job(body,'succeeded');break;
  case 'codex_queue':state.login={status:'queued'};job(body);break;
  case 'codex_launch':state.login={status:'connecting'};job(body);break;
  case 'setup_close':job(body);break;
  case 'refresh':case 'reconnect':job(body);break;
  default:return send(response,400,{ok:false,error_code:'invalid_request'});
 }
 const result={ok:true,status:'queued',request_id:body.request_id};idempotent.set(body.request_id,result);if(idempotent.size>32)idempotent.delete(idempotent.keys().next().value);send(response,200,result);
});
server.listen(port,'127.0.0.1',()=>process.stdout.write('Synthetic preview: http://127.0.0.1:'+port+'/#s='+secret+'\nScenarios: /__preview/empty /__preview/offline /__preview/expired /__preview/pending /__preview/companion\n'));

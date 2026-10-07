// Manually started synthetic loopback fixture; never contacts a device/provider.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const htmlPath=path.join(root,'main/portable_setup.html');
const port=4328,secret='s'.repeat(43),epoch=()=>Math.floor(Date.now()/1000);
let scenario=process.argv.find(item=>item.startsWith('--scenario='))?.slice(11)||'default',state,pendingNetwork;
const seedAccounts=()=>[
 {id:'fixture-codex',provider:'codex',label:'Personal Codex',email:'preview@example.invalid',plan:'Plus',status:'ok',auth_state:'ready',observed_at:epoch()-120,five_hour:{present:true,remaining_percent:68,resets_at:epoch()+9000},seven_day:{present:true,remaining_percent:42,resets_at:epoch()+320000}},
 {id:'fixture-deepseek',provider:'deepseek',label:'DeepSeek API',status:'ok',auth_state:'ready',observed_at:epoch()-180,balance:{is_available:true,balance_infos:[{currency:'CNY',total_balance:'123.456700'}]}},
];
function reset(){
 state={session:{active:scenario!=='expired',remaining_seconds:scenario==='expired'?0:590},network:{state:scenario==='offline'?'error':'ap',connected:false,ssid:'Preview Wi-Fi',ip:'',saved_networks:[{index:0,ssid:'Preview Wi-Fi',selected:true},{index:1,ssid:'Phone Hotspot',selected:false},{index:2,ssid:'Other Saved Wi-Fi',selected:false}]},clock:{synchronized:scenario!=='offline',epoch:epoch()},settings:{auto_refresh:true,refresh_seconds:300,screen_timeout_seconds:120},accounts:seedAccounts(),jobs:[],operation:{request_id:'',kind:'none',state:'queued',seconds_left:0,cancelable:false}};pendingNetwork=null;
 if(scenario==='empty'){state.accounts=[];state.network.saved_networks=[];state.network.ssid='';}
 if(scenario==='full')state.accounts=Array.from({length:8},(_,k)=>({...seedAccounts()[k%2],id:'fixture-active-'+k,label:'Active '+(k+1)}));
 if(scenario==='pending'||scenario==='saving'){state.operation={request_id:'aaaa0001',kind:'key',state:scenario==='saving'?'saving':'waiting',seconds_left:650,cancelable:scenario!=='saving'};state.jobs=[{request_id:'aaaa0001',op:'deepseek_save',status:'running',waiting:true}];}
 if(scenario==='transport'){state.accounts[0].error_code='resource_error';state.accounts[0].status='error';state.accounts[1].error_code='rate_limited';state.accounts[1].retry_after_seconds=45;state.accounts[1].retry_at=state.clock.epoch+45;}
}
reset();
const idempotent=new Map(),all=()=>state.accounts;
function send(response,status,data){response.writeHead(status,{'Content-Type':'application/json;charset=utf-8','Cache-Control':'no-store'});response.end(JSON.stringify(data));}
function job(body,status='queued',error_code=''){state.jobs.push({request_id:body.request_id,op:body.op,status,...(error_code?{error_code}:{})});state.jobs=state.jobs.slice(-4);}
const server=http.createServer(async(request,response)=>{
 const url=new URL(request.url,'http://127.0.0.1:'+port);
 if(url.pathname.startsWith('/__preview/')){scenario=url.pathname.slice(11);reset();idempotent.clear();response.writeHead(302,{Location:scenario==='manual'?'/':'/#s='+secret});response.end();return;}
 if(url.pathname==='/'){response.writeHead(200,{'Content-Type':'text/html;charset=utf-8','Cache-Control':'no-store','Referrer-Policy':'no-referrer','Content-Security-Policy':"default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"});response.end(fs.readFileSync(htmlPath));return;}
 if(['/portable_setup.mjs','/portable_serial.mjs'].includes(url.pathname)&&request.method==='GET'){response.writeHead(200,{'Content-Type':'text/javascript;charset=utf-8','Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});response.end(fs.readFileSync(path.join(root,'main',url.pathname.slice(1))));return;}
 if(request.headers['x-aiq-setup']!==secret)return send(response,401,{ok:false,error_code:'unauthorized'});
 if(!state.session.active)return send(response,401,{ok:false,error_code:'session_expired'});
 if(url.pathname==='/api/state'&&request.method==='GET')return send(response,200,state);
 if(url.pathname!=='/api/command'||request.method!=='POST')return send(response,404,{ok:false,error_code:'not_found'});
 let input='';for await(const chunk of request){input+=chunk;if(input.length>4096)return send(response,413,{ok:false,error_code:'invalid_request'});}
 let body;try{body=JSON.parse(input);}catch{return send(response,400,{ok:false,error_code:'invalid_request'});}
 if(body.v!==1||!/^[a-f0-9]{8}$/.test(body.request_id||''))return send(response,400,{ok:false,error_code:'invalid_request'});
 if(idempotent.has(body.request_id))return send(response,200,idempotent.get(body.request_id));
 const fail=code=>job(body,'failed',code);
 switch(body.op){
  case 'network_save':pendingNetwork=body;job(body);break;
  case 'deepseek_save':{const account=all().find(item=>item.id===body.account_id);if(account&&!body.api_key){account.label=body.label;job(body,'succeeded');break;}if(!account&&state.accounts.length>=8){fail('account_limit');break;}job(body,'running');state.jobs.at(-1).waiting=true;state.operation={request_id:body.request_id,kind:'key',state:'waiting',seconds_left:650,cancelable:true};break;}
  case 'account_remove':state.accounts=state.accounts.filter(item=>item.id!==body.account_id);job(body,'succeeded');break;
  case 'settings_save':state.settings={auto_refresh:body.auto_refresh,refresh_seconds:body.refresh_seconds,screen_timeout_seconds:body.screen_timeout_seconds};job(body,'succeeded');break;
  case 'mode_select':return send(response,410,{ok:false,error_code:'unsupported'});
  case 'codex_queue':state.operation={request_id:body.request_id,kind:'login',state:'queued',seconds_left:900,cancelable:true};job(body,'succeeded');break;
  case 'codex_launch':state.operation={request_id:body.request_id,kind:'login',state:'connecting',seconds_left:900,cancelable:true};job(body,'running');break;
  case 'setup_close':if(pendingNetwork){const saved=state.network.saved_networks;let index=pendingNetwork.network_index;if(index===undefined)index=saved.findIndex(item=>item.ssid===pendingNetwork.ssid);if(index<0||index===undefined)index=saved.length;const old=saved.find(item=>item.index===index);if(index<3){if(pendingNetwork.ssid){if(old)old.ssid=pendingNetwork.ssid;else saved.push({index,ssid:pendingNetwork.ssid});}saved.forEach(item=>item.selected=item.index===index);state.network.ssid=saved.find(item=>item.selected)?.ssid||'';}pendingNetwork=null;}job(body,'succeeded');break;
  case 'operation_cancel':{const target=state.jobs.find(item=>item.request_id===body.target_request_id);if(target&&state.operation.cancelable&&state.operation.request_id===body.target_request_id){target.status='failed';target.error_code='canceled';target.waiting=false;state.operation={request_id:'',kind:'none',state:'queued',seconds_left:0,cancelable:false};job(body,'succeeded');}else fail('invalid_request');break;}
  case 'refresh':case 'reconnect':job(body);break;
  default:return send(response,400,{ok:false,error_code:'invalid_request'});
 }
 const result={accepted:true,request_id:body.request_id};idempotent.set(body.request_id,result);if(idempotent.size>32)idempotent.delete(idempotent.keys().next().value);send(response,202,result);
});
server.listen(port,'127.0.0.1',()=>process.stdout.write('Synthetic preview: http://127.0.0.1:'+port+'/#s='+secret+'\nScenarios: /__preview/manual /__preview/empty /__preview/offline /__preview/expired /__preview/pending /__preview/saving /__preview/full /__preview/transport\n'));

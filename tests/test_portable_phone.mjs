// Execute the production shared setup page against a synthetic in-memory DOM.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';
const html=fs.readFileSync(new URL('../main/portable_setup.html',import.meta.url),'utf8');
const source=fs.readFileSync(new URL('../main/portable_setup.mjs',import.meta.url),'utf8').replace(/^import .*;\s*/, '');
const secret='s'.repeat(43);
new vm.Script(source);
assert(!/localStorage|sessionStorage|indexedDB/.test(source),'secrets have no browser storage path');
assert(!/mode_select|id="mode"|switch-direct/.test(html),'shared setup has no global source selector');
assert(html.includes('src="./portable_setup.mjs"'),'embedded page loads the shared external module');
assert(!/<script>/.test(html),'shared setup has no inline script');
function harness({usb=false,usbSession=null}={}){
 const nodes=new Map(),windowEvents=new Map();
 const node=id=>{if(!nodes.has(id))nodes.set(id,{id,value:'',innerHTML:'',textContent:'',className:'',hidden:false,disabled:false,checked:false,dataset:{},classList:{toggle(){}},setAttribute(){},querySelectorAll(){return [];},querySelector(){return node(id+'-button');}});return nodes.get(id);};
 for(const match of html.matchAll(/id="([^"]+)"/g))node(match[1]);
 const tabs=['accounts','network','settings'].map(tab=>({dataset:{tab},classList:{toggle(){}},setAttribute(){}}));
 const document={hidden:false,getElementById:node,querySelectorAll:selector=>selector==='[data-tab]'?tabs:selector==='button,input,select'?[...nodes.values()]:[],addEventListener(){}};
 const context={document,addEventListener:(name,handler)=>windowEvents.set(name,handler),location:{hash:'',search:usb?'?transport=usb':'',pathname:'/',hostname:'localhost'},history:{replaceState(_a,_b,path){context.cleaned=path;}},URL,URLSearchParams,TextEncoder,Date,crypto:webcrypto,navigator:{serial:{requestPort:async()=>({})}},DeviceSerialError:class extends Error{constructor(code){super(code);this.code=code;}},serialErrorMessage:error=>error?.code||null,makeUsbRequestId:()=>'1234abcd',openUsbDeviceSession:async()=>usbSession,setInterval(){},setTimeout:callback=>callback(),fetch:async()=>{throw Error('unexpected fetch');}};
 vm.createContext(context);vm.runInContext(source+`\nglobalThis.test={render,jobText,accountStatus,command,poll,launchCodex,startSession,connectUsbPort,connectUsbSession,configureCollector,setup(stateValue){state=stateValue;stopped=false;busy=false;launchBusy=false;setupSecret='${secret}';},select(id){selectedId=id;confirmation=null;removingId='';},getSecret(){return setupSecret;},hasSession};`,context);
 return {context,node,test:context.test,emitWindow:name=>windowEvents.get(name)?.()};
}
const account=(id='synthetic-account',provider='deepseek',extra={})=>({id,provider,label:'Synthetic '+id,source:'device',status:'ok',balance:{balance_infos:[{currency:'CNY',total_balance:'123.456700'}]},...extra});
const state=()=>({session:{remaining_seconds:590},network:{connected:true,state:'connected',ssid:'Synthetic Wi-Fi',saved_networks:[{index:0,ssid:'Synthetic Wi-Fi',selected:true},{index:1,ssid:'Other synthetic Wi-Fi',selected:false}]},clock:{synchronized:true,epoch:1800000000},settings:{auto_refresh:true,refresh_seconds:300,screen_timeout_seconds:120},accounts:[account()],pending_accounts:[],collector:{configured:true,connected:false,epoch:1,discovery:[]},jobs:[],operation:{kind:'none'}});
const response=(value,status=200)=>({ok:status>=200&&status<300,status,json:async()=>value});
const click=id=>({target:{id,closest(){return null;}}});
function capture(context,s){const posts=[];context.fetch=async(_path,options)=>{if(options.method==='POST'){posts.push(JSON.parse(options.body));return response({accepted:true,request_id:posts.at(-1).request_id},202);}return response(s);};return posts;}
{
 const {context,node,test}=harness();assert.equal(context.cleaned,'/');assert.equal(test.getSecret(),''); const s=state();let requests=0;context.fetch=async(_path,options)=>{requests++;assert.equal(options.headers['X-AIQ-Setup'],secret);return response(s);};
 assert.equal(await test.startSession('bad'),false);assert.equal(requests,0);assert.equal(node('session-secret').value,'');
 assert.equal(await test.startSession(secret.slice(0,11)+'\n'+secret.slice(11,22)+' '+secret.slice(22)),true);assert.equal(node('session-form').hidden,true);assert.equal(node('session-content').hidden,false);assert.equal(test.getSecret(),secret);assert.equal(node('message').textContent,'已连接设备设置');
 context.fetch=async()=>response({error_code:'unauthorized'},403);await test.poll();assert.equal(test.getSecret(),'');assert.equal(node('session-form').hidden,false);assert.equal(node('session-secret').disabled,false);
}
{
 const {context,node,test}=harness();context.fetch=async()=>{throw Error('offline');};assert.equal(await test.startSession(secret),false);assert(!node('message').textContent.includes('已连接设备设置'),'failed manual connection must not claim success');
}
{
 const {context,node,test}=harness();const s=state();s.accounts.push(account('claude','claude',{source:'legacy'}));test.setup(s);test.render();
 assert.equal(node('network-slot-field').hidden,false);assert(node('saved-networks').innerHTML.includes('Other synthetic Wi-Fi'));assert.equal(node('refresh').disabled,false);assert(node('account-detail').innerHTML.includes('replace-key-form'));assert(node('account-list').innerHTML.includes('123.456700'));assert(node('account-list').innerHTML.includes('Passport 自行更新'));assert(node('account-list').innerHTML.includes('电脑采集器更新'));
 node('refresh-seconds').value='60';test.render();assert.equal(node('refresh-seconds').value,'60','polling preserves drafts');
 s.accounts[1].auth_state='pending';s.collector.connected=true;assert.equal(test.accountStatus(s.accounts[1]),'已连接','legacy status must not use device credential auth metadata');s.collector.connected=false;assert(test.accountStatus(s.accounts[1]).includes('采集器离线'));
 test.select('claude');test.render();assert(node('account-detail').innerHTML.includes('Claude 原生授权'));assert(!node('account-detail').innerHTML.includes('id="reauthorize"'));
 assert(test.accountStatus({error_code:'resource_error'}).includes('资源不足'));assert(!test.accountStatus({error_code:'resource_error'}).includes('Wi-Fi'));assert(test.accountStatus({error_code:'rate_limited',retry_at:s.clock.epoch+45}).includes('45 秒'));
 s.operation={kind:'save',state:'saving',request_id:'0123abcd',cancelable:false};test.render();assert(node('pending-login').innerHTML.includes('正在保存'));assert(!node('pending-login').innerHTML.includes('operation-cancel'));
 s.operation={kind:'key',state:'waiting',request_id:'0123abcd',cancelable:true,seconds_left:60};test.render();assert(node('pending-login').innerHTML.includes('operation-cancel'));
 assert(test.jobText({op:'settings_save',status:'running',waiting:true}).includes('尚未生效'));assert(test.jobText({op:'settings_save',status:'succeeded'}).includes('已生效'));assert(test.jobText({status:'failed',error_code:'pending_account_limit'}).includes('双方账户'));assert(test.jobText({status:'failed',error_code:'storage_write_unknown'}).includes('确认保存结果'));
 const posts=capture(context,s);await node('pending-login').onclick(click('operation-cancel'));await new Promise(resolve=>setImmediate(resolve));assert.equal(posts[0].op,'operation_cancel');assert.equal(posts[0].target_request_id,'0123abcd');assert(Number.isInteger(posts[0].phone_utc));
}
{
 const {context,node,test}=harness(),s=state();s.accounts=Array.from({length:8},(_,i)=>account('active-'+i));s.pending_accounts=Array.from({length:8},(_,i)=>account('history-'+i));s.collector.discovery=[{remote_account_id:'remote-one',provider:'claude',label:'Computer Claude'}];test.setup(s);test.select('history-0');test.render();const posts=capture(context,s);
 s.collector.discovery.push({remote_account_id:'already-linked',provider:'codex',label:'Existing Codex',imported:true});test.render();assert(node('collector-accounts').innerHTML.includes('已导入'));assert(!node('collector-accounts').innerHTML.includes('data-import="already-linked"'));
 assert.equal(node('add-toggle').disabled,true);assert(node('pending-accounts').innerHTML.includes('历史账户 · 8/8'));
 await node('account-detail').onclick(click('activate-account'));assert.equal(posts.length,0);assert(node('account-detail').innerHTML.includes('保留凭证'));
 await node('account-detail').onclick(click('activate-confirm'));assert.equal(posts.length,0,'full catalog activation requires explicit replacement');node('replace-active').value='active-0';await node('account-detail').onclick(click('activate-confirm'));assert.equal(posts[0].op,'account_activate');assert.equal(posts[0].account_id,'history-0');assert.equal(posts[0].replace_active_id,'active-0');assert(!posts.some(item=>item.op==='account_remove'));
}
{
 const {context,node,test}=harness(),s=state();s.accounts=[account('old-source','codex',{source:'legacy',source_changed:true})];test.setup(s);test.render();const posts=capture(context,s);
 await node('account-detail').onclick(click('activate-account'));assert.equal(posts.length,0);assert(node('account-detail').innerHTML.includes('当前配对电脑'));await node('account-detail').onclick(click('activate-confirm'));assert.equal(posts[0].op,'account_activate');assert(!Object.hasOwn(posts[0],'endpoint_epoch'));
 await node('account-detail').onclick(click('upgrade-account'));assert.equal(posts.length,1);assert(node('account-detail').innerHTML.includes('替换此账户的电脑更新来源'));
 await node('account-detail').onclick(click('remove'));assert(node('account-detail').innerHTML.includes('电脑中的授权仍会保留'));
}
{
 const {context,node,test}=harness(),s=state();s.collector.discovery=[{remote_account_id:'remote-one',provider:'claude',label:'Computer Claude'}];s.network.pending_network={ssid:'Preserved fourth network'};test.setup(s);test.render();const posts=capture(context,s);
 await node('collector-accounts').onclick({target:{closest:()=>({dataset:{import:'remote-one'}})}});assert.equal(posts.length,0);assert(node('collector-accounts').innerHTML.includes('不会导入凭证'));await node('collector-accounts').onclick(click('import-confirm'));assert.equal(posts[0].op,'external_import');assert.equal(posts[0].remote_account_id,'remote-one');assert(!Object.hasOwn(posts[0],'provider'));
 node('pending-network').onclick(click('pending-network-activate'));assert.equal(posts.length,1);node('pending-network-replace').value='1';node('pending-network').onclick(click('pending-network-activate'));await new Promise(resolve=>setImmediate(resolve));assert.equal(posts[1].op,'network_activate');assert.equal(posts[1].replace_index,1);assert(node('pending-network').innerHTML.includes('被替换网络会保留'));
}
for(const failed of [false,true]){
 const {context,node,test}=harness(),s=state();test.setup(s);test.render();const posts=[];context.fetch=async(_path,options)=>{if(options.method==='POST'){const body=JSON.parse(options.body);posts.push(body);if(body.op==='codex_queue')s.jobs=[{request_id:body.request_id,op:body.op,status:failed?'failed':'succeeded',error_code:failed?'configuration_changed':''}];return response({accepted:true,request_id:body.request_id},202);}return response(s);};
 await test.launchCodex('');assert.deepEqual(posts.map(item=>item.op),failed?['codex_queue']:['codex_queue','codex_launch']);assert.equal(test.getSecret(),failed?secret:'');assert(failed?node('message').textContent.includes('配置已变化'):node('message').textContent.includes('手机或电脑'));
}
for(const reject of [false,true]){
 const {context,node,test}=harness(),s=state();test.setup(s);test.render();let finish;context.fetch=async(_path,options)=>options.method==='POST'?response({accepted:true,request_id:'1234abcd'},202):new Promise((resolve,rejectPromise)=>finish=()=>reject?rejectPromise(Error('cutover')):resolve(response(s)));
 const pending=test.poll();await test.command('setup_close',{},'ACK：查看设备',true);finish();await pending;assert.equal(test.getSecret(),'');assert.equal(node('message').textContent,'ACK：查看设备','in-flight polling preserves acknowledged AP cutover');
}
{
 const s=state();s.collector.discovery=[{remote_account_id:'remote-claude',provider:'claude',label:'Synthetic Claude'}];
 const sent=[];let jobs=[];
 const usb={async openSession(){return {session_id:'a'.repeat(32),remaining_seconds:90};},async stateGet(){return {...s,jobs};},jobProvesAdmission(_state,id){return jobs.some(job=>job.request_id===id);},async command(body){sent.push({type:'command',body});if(body.op==='codex_queue')jobs=[{request_id:body.request_id,op:body.op,status:'succeeded'}];if(body.op==='codex_launch')s.login={state:'waiting',verification_url:'https://attacker.example/',user_code:'ABCD-EFGH',seconds_left:75};if(body.op==='external_import')jobs=[{request_id:body.request_id,op:body.op,status:'succeeded'}];return {accepted:true,request_id:body.request_id};},async collectorConfigure(endpoint,id){sent.push({type:'collector_configure',endpoint,id});jobs=[{request_id:id,op:'collector_configure',status:'succeeded'}];return {accepted:true,request_id:id};},async close(){}};
 const {context,node,test}=harness({usb:true,usbSession:usb});const hostRequests=[];
 context.fetch=async(path,options={})=>{hostRequests.push({path,options});if(path==='/api/state')return response({csrf_token:'synthetic-csrf',interfaces:[{name:'Wi-Fi',address:'192.168.1.8'}],device:{}});if(path==='/api/pairing')return response({base_url:'https://192.168.1.8:4318',pair_token:'p'.repeat(43),server_cert_pem:'CERTIFICATE '.repeat(60),server_time:1800000000,pairing_session:'pair-1'});if(path==='/api/pairing/abort')return response({ok:true});throw Error(`unexpected host API ${path}`);};
 await test.connectUsbPort();await test.connectUsbSession();assert.equal(test.hasSession(),true);assert.equal(node('session-form').hidden,true);assert.equal(node('session-content').hidden,false);assert(hostRequests.some(item=>item.path==='/api/state'&&item.options.method==='GET'));
 s.login={state:'waiting',verification_url:'https://attacker.example/',user_code:'ABCD-EFGH',seconds_left:75};await test.poll();assert(node('pending-login').innerHTML.includes('https://auth.openai.com/codex/device'));assert(!node('pending-login').innerHTML.includes('attacker.example'));assert(node('pending-login').innerHTML.includes('ABCD-EFGH'));
 await test.command('deepseek_save',{api_key:'synthetic-secret-key',label:'Synthetic DeepSeek'});assert.equal(sent.at(-1).body.api_key,'synthetic-secret-key');assert(!hostRequests.some(item=>JSON.stringify(item.options.body||'').includes('synthetic-secret-key')),'provider keys go only through USB');assert.equal(node('deepseek-key').value,'');
 test.render();node('collector-address').value='192.168.1.8';await test.configureCollector();assert(hostRequests.some(item=>item.path==='/api/pairing'),node('message').textContent);const pairing=sent.find(item=>item.type==='collector_configure');assert(pairing,node('message').textContent);assert.deepEqual(Object.keys(pairing.endpoint).sort(),['base_url','pair_token','server_cert_pem','server_time']);assert(!hostRequests.some(item=>item.path==='/api/pairing/abort'),'successful USB collector configuration keeps its pairing');
 await node('collector-accounts').onclick({target:{id:'',closest:()=>({dataset:{import:'remote-claude'}})}});assert.equal(sent.filter(item=>item.body?.op==='external_import').length,0,'discovery does not import automatically');await node('collector-accounts').onclick(click('import-confirm'));assert.equal(sent.at(-1).body.op,'external_import');assert.equal(sent.at(-1).body.remote_account_id,'remote-claude');
 await test.launchCodex('');assert.deepEqual(sent.slice(-2).map(item=>item.body.op),['codex_queue','codex_launch']);assert.equal(test.hasSession(),true,'Codex login keeps the USB settings session open');
}
{
 const s=state();const sent=[];const usb={async openSession(){return {session_id:'b'.repeat(32),remaining_seconds:90};},async stateGet(){return s;},jobProvesAdmission(){return false;},async collectorConfigure(endpoint,id){sent.push({endpoint,id});throw Object.assign(new Error('expired'),{code:'device_session_expired'});},async close(){}};
 const {context,node,test}=harness({usb:true,usbSession:usb});const hostRequests=[];
 context.fetch=async(path,options={})=>{hostRequests.push({path,options});if(path==='/api/state')return response({csrf_token:'synthetic-csrf',interfaces:[{name:'Wi-Fi',address:'192.168.1.8'}],device:{}});if(path==='/api/pairing')return response({base_url:'https://192.168.1.8:4318',pair_token:'q'.repeat(43),server_cert_pem:'CERTIFICATE '.repeat(60),server_time:1800000000,pairing_session:'pair-expired-ack'});if(path==='/api/pairing/abort')return response({ok:true});throw Error(`unexpected host API ${path}`);};
 await test.connectUsbPort();await test.connectUsbSession();node('collector-address').value='192.168.1.8';await test.configureCollector();
 assert.equal(sent.length,1,'collector configuration was transmitted before session expiry was reported');assert(!hostRequests.some(item=>item.path==='/api/pairing/abort'),'a possibly saved pairing is retained after transmission');assert(node('message').textContent.includes('可能已保存'));assert(node('message').textContent.includes('勿撤销或重新配对'));assert(node('message').textContent.includes('读取状态核对'));
}
{
 const s=state();let testRef;let reads=0,resets=0,closes=0;const sent=[];
 const usb={async openSession(){return {session_id:'c'.repeat(32),remaining_seconds:90};},resetSession(){resets++;},async stateGet(){reads++;if(reads===2)throw Object.assign(new Error('timeout'),{code:'serial_timeout'});if(reads===3)await testRef.command('settings_save',{auto_refresh:true});return s;},jobProvesAdmission(){return false;},async command(body){sent.push(body);if(body.op==='settings_save'&&sent.length===1)throw Object.assign(new Error('expired'),{code:'device_session_expired'});return {accepted:true,request_id:body.request_id};},async collectorConfigure(){throw Error('unexpected collector configuration');},async close(){closes++;}};
 const {context,node,test}=harness({usb:true,usbSession:usb});testRef=test;context.fetch=async path=>{if(path==='/api/state')return response({csrf_token:'synthetic-csrf',interfaces:[],device:{}});throw Error(`unexpected host API ${path}`);};
 await test.connectUsbPort();await test.connectUsbSession();assert.equal(reads,1);assert.equal(test.hasSession(),true);
 assert.equal(await test.command('settings_save',{auto_refresh:true}),null);assert.equal(test.hasSession(),false);assert.equal(closes,0,'session expiry keeps the same serial port open');assert.equal(node('usb-session').hidden,false);assert(node('usb-hint').textContent.includes('请勿断开 USB'));
 await test.connectUsbSession();assert.equal(resets,1);assert.equal(reads,2);assert.equal(sent.length,1,'old mutation is never replayed');assert.equal(test.hasSession(),false,'a failed state read does not enable mutations');assert.equal(closes,0);
 await test.connectUsbSession();assert.equal(resets,1,'a retry after a state timeout reuses the confirmed opener and session');assert.equal(reads,3);assert.equal(sent.length,1,'the new window is read before another mutation is allowed');assert.equal(test.hasSession(),true);assert.equal(closes,0);
 await node('reconnect').onclick();assert.equal(sent.at(-1).op,'reconnect');assert.equal(test.hasSession(),true,'USB reconnect keeps the settings window and port open');assert.equal(closes,0);
 await node('close-setup').onclick();assert.equal(sent.at(-1).op,'setup_close');assert.equal(closes,1,'explicit setup_close closes the USB port');
}
for(const usbMode of [false,true]){
 let closes=0;const s=state();
 const usb={async openSession(){},async stateGet(){return s;},async close(){closes++;}};
 const {context,node,test,emitWindow}=harness({usb:usbMode,usbSession:usb});
 context.fetch=async()=>response(usbMode?{csrf_token:'synthetic-csrf',interfaces:[],device:{}}:s);
 if(usbMode){await test.connectUsbPort();await test.connectUsbSession();}else await test.startSession(secret);
 for(const id of ['session-secret','wifi-password','deepseek-key','replace-key'])node(id).value='synthetic-secret';
 emitWindow('pagehide');
 assert.equal(test.hasSession(),false);assert.equal(test.getSecret(),'');assert.equal(node('session-content').hidden,true);
 for(const id of ['session-secret','wifi-password','deepseek-key','replace-key'])assert.equal(node(id).value,'');
 assert.equal(closes,usbMode?1:0,'the window pagehide event releases USB without another request');
}
console.log('Shared device page: AP regression and USB-native settings, secret routing, fixed Codex URL, collector pairing and explicit import PASS');

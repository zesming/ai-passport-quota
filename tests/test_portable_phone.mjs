// Execute the production shared setup page against a synthetic in-memory DOM.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';
import { UsbDeviceSession, startDeviceSerial, serialErrorMessage as deviceSerialMessage } from '../main/portable_serial.mjs';
const html=fs.readFileSync(new URL('../main/portable_setup.html',import.meta.url),'utf8');
const source=fs.readFileSync(new URL('../main/portable_setup.mjs',import.meta.url),'utf8').replace(/^import .*;\s*/, '');
const secret='s'.repeat(43);
new vm.Script(source);
assert(!/localStorage|sessionStorage|indexedDB/.test(source),'secrets have no browser storage path');
assert(!/mode_select|id="mode"|switch-direct/.test(html),'shared setup has no global source selector');
assert(html.includes('src="./portable_setup.mjs"'),'embedded page loads the shared external module');
assert(!/<script>/.test(html),'shared setup has no inline script');
function harness({usb=false,usbSession=null}={}){
 const nodes=new Map(),windowEvents=new Map(),intervals=[];
 const node=id=>{if(!nodes.has(id))nodes.set(id,{id,value:'',innerHTML:'',textContent:'',className:'',hidden:false,disabled:false,checked:false,dataset:{},classList:{toggle(){}},setAttribute(){},querySelectorAll(){return [];},querySelector(){return node(id+'-button');}});return nodes.get(id);};
 for(const match of html.matchAll(/id="([^"]+)"/g))node(match[1]);
 const tabs=['accounts','network','settings'].map(tab=>({dataset:{tab},classList:{toggle(){}},setAttribute(){}}));
 const document={hidden:false,getElementById:node,querySelectorAll:selector=>selector==='[data-tab]'?tabs:selector==='button,input,select'?[...nodes.values()]:[],addEventListener(){}};
 const context={document,addEventListener:(name,handler)=>windowEvents.set(name,handler),location:{hash:'',search:usb?'?transport=usb':'',pathname:'/',hostname:'localhost'},history:{replaceState(_a,_b,path){context.cleaned=path;}},URL,URLSearchParams,TextEncoder,Date,crypto:webcrypto,navigator:{serial:{requestPort:async()=>({})}},DeviceSerialError:class extends Error{constructor(code){super(code);this.code=code;}},serialErrorMessage:error=>error?.code||null,openUsbDeviceSession:async()=>usbSession,setInterval(callback){intervals.push(callback);},setTimeout:callback=>callback(),fetch:async()=>{throw Error('unexpected fetch');}};
 vm.createContext(context);vm.runInContext(source+`\nglobalThis.test={render,jobText,accountStatus,command,poll,launchCodex,startSession,connectUsbPort,connectUsbSession,setup(stateValue){state=stateValue;stopped=false;busy=false;launchBusy=false;setupSecret='${secret}';},select(id){selectedId=id;removingId='';},getSecret(){return setupSecret;},hasSession};`,context);
 return {context,document,node,test:context.test,emitWindow:name=>windowEvents.get(name)?.(),tickIntervals:async()=>{intervals.forEach(callback=>callback());await new Promise(resolve=>setImmediate(resolve));}};
}
const account=(id='synthetic-account',provider='deepseek',extra={})=>({id,provider,label:'Synthetic '+id,status:'ok',balance:{balance_infos:[{currency:'CNY',total_balance:'123.456700'}]},...extra});
const state=()=>({session:{remaining_seconds:590},network:{connected:true,state:'connected',ssid:'Synthetic Wi-Fi',saved_networks:[{index:0,ssid:'Synthetic Wi-Fi',selected:true},{index:1,ssid:'Other synthetic Wi-Fi',selected:false}]},clock:{synchronized:true,epoch:1800000000},settings:{auto_refresh:true,refresh_seconds:300,screen_timeout_seconds:120},accounts:[account()],jobs:[],operation:{kind:'none'}});
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
 const {context,node,test}=harness();const s=state();s.accounts.push(account('codex-two','codex'));test.setup(s);test.render();
 assert.equal(node('network-slot-field').hidden,false);assert(node('saved-networks').innerHTML.includes('Other synthetic Wi-Fi'));assert.equal(node('refresh').disabled,false);assert(node('account-detail').innerHTML.includes('replace-key-form'));assert(node('account-list').innerHTML.includes('123.456700'));assert(!node('account-list').innerHTML.includes('电脑'));
 node('refresh-seconds').value='60';test.render();assert.equal(node('refresh-seconds').value,'60','polling preserves drafts');
 s.accounts[1].auth_state='pending';assert.equal(test.accountStatus(s.accounts[1]),'待验证');s.accounts[1].auth_state='expired';assert.equal(test.accountStatus(s.accounts[1]),'需要重新授权');s.accounts[1].auth_state='ready';assert.equal(test.accountStatus(s.accounts[1]),'已连接');
 test.select('codex-two');test.render();assert(node('account-detail').innerHTML.includes('id="reauthorize"'));assert(!node('account-detail').innerHTML.includes('activate'));
 assert(test.accountStatus({error_code:'resource_error'}).includes('资源不足'));assert(!test.accountStatus({error_code:'resource_error'}).includes('Wi-Fi'));assert(test.accountStatus({error_code:'rate_limited',retry_at:s.clock.epoch+45}).includes('45 秒'));
 s.operation={kind:'save',state:'saving',request_id:'0123abcd',cancelable:false};test.render();assert(node('pending-login').innerHTML.includes('正在保存'));assert(!node('pending-login').innerHTML.includes('operation-cancel'));
 s.operation={kind:'key',state:'waiting',request_id:'0123abcd',cancelable:true,seconds_left:60};test.render();assert(node('pending-login').innerHTML.includes('operation-cancel'));
 assert(test.jobText({op:'settings_save',status:'running',waiting:true}).includes('尚未生效'));assert(test.jobText({op:'settings_save',status:'succeeded'}).includes('已生效'));assert(test.jobText({status:'failed',error_code:'account_limit'}).includes('最多 8 个账户'));assert(test.jobText({status:'failed',error_code:'storage_write_unknown'}).includes('确认保存结果'));
 const posts=capture(context,s);await node('pending-login').onclick(click('operation-cancel'));await new Promise(resolve=>setImmediate(resolve));assert.equal(posts[0].op,'operation_cancel');assert.equal(posts[0].target_request_id,'0123abcd');assert(Number.isInteger(posts[0].phone_utc));
}
{
 const {context,node,test}=harness(),s=state();s.accounts=Array.from({length:8},(_,i)=>account('active-'+i));test.setup(s);test.render();
 assert.equal(node('add-toggle').disabled,true);assert(node('account-list').innerHTML.includes('账户 · 8/8'));assert(!node('account-detail').innerHTML.includes('activate'),'a full catalog has no hidden rows to activate');
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
 const s=state();
 const sent=[];let jobs=[];
 const usb={async openSession(){return {session_id:'a'.repeat(32),remaining_seconds:90};},async stateGet(){return {...s,jobs};},jobProvesAdmission(_state,id){return jobs.some(job=>job.request_id===id);},async command(body){sent.push({type:'command',body});if(body.op==='codex_queue')jobs=[{request_id:body.request_id,op:body.op,status:'succeeded'}];if(body.op==='codex_launch')s.login={state:'waiting',verification_url:'https://attacker.example/',user_code:'ABCD-EFGH',seconds_left:75};return {accepted:true,request_id:body.request_id};},async close(){}};
 const {context,node,test}=harness({usb:true,usbSession:usb});const hostRequests=[];
 context.fetch=async(path,options={})=>{hostRequests.push({path,options});throw Error(`unexpected network request ${path}`);};
 await test.connectUsbPort();await test.connectUsbSession();assert.equal(test.hasSession(),true);assert.equal(node('session-form').hidden,true);assert.equal(node('session-content').hidden,false);assert.equal(hostRequests.length,0,'USB settings make no network request');
 s.login={state:'waiting',verification_url:'https://attacker.example/',user_code:'ABCD-EFGH',seconds_left:75};await test.poll();assert(node('pending-login').innerHTML.includes('https://auth.openai.com/codex/device'));assert(!node('pending-login').innerHTML.includes('attacker.example'));assert(node('pending-login').innerHTML.includes('ABCD-EFGH'));
 await test.command('deepseek_save',{api_key:'synthetic-secret-key',label:'Synthetic DeepSeek'});assert.equal(sent.at(-1).body.api_key,'synthetic-secret-key');assert.equal(hostRequests.length,0,'provider keys go only through USB');assert.equal(node('deepseek-key').value,'');
 await test.launchCodex('');assert.deepEqual(sent.slice(-2).map(item=>item.body.op),['codex_queue','codex_launch']);assert.equal(test.hasSession(),true,'Codex login keeps the USB settings session open');
}
{
 const s=state();let testRef;let reads=0,resets=0,closes=0;const sent=[];
 const usb={async openSession(){return {session_id:'c'.repeat(32),remaining_seconds:90};},resetSession(){resets++;},async stateGet(){reads++;if(reads===2)throw Object.assign(new Error('timeout'),{code:'serial_timeout'});if(reads===3)await testRef.command('settings_save',{auto_refresh:true});return s;},jobProvesAdmission(){return false;},async command(body){sent.push(body);if(body.op==='settings_save'&&sent.length===1)throw Object.assign(new Error('expired'),{code:'device_session_expired'});return {accepted:true,request_id:body.request_id};},async close(){closes++;}};
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
{
 /* Wi-Fi password rules match the device: 8-63 bytes, or exactly 64 hex digits; empty only for an open network. */
 const {context,node,test}=harness(),s=state();test.setup(s);test.render();const posts=capture(context,s);
 const submit=async(password,open=false)=>{posts.length=0;node('ssid').value='Synthetic Wi-Fi';node('wifi-password').value=password;node('open-network').checked=open;node('network-slot').value='';await node('network-form').onsubmit({preventDefault(){}});await new Promise(resolve=>setImmediate(resolve));return posts.length===1;};
 assert.equal(await submit('abc1234'),false);assert(node('message').textContent.includes('8 至 63'));
 assert.equal(await submit('中中中'),true,'9 UTF-8 bytes satisfy the device minimum');assert.equal(posts[0].password,'中中中');
 assert.equal(await submit('x'.repeat(63)),true);
 assert.equal(await submit('z'.repeat(64)),false);assert(node('message').textContent.includes('十六进制'));
 assert.equal(await submit('aB09'.repeat(16)),true);
 assert.equal(await submit('x'.repeat(65)),false);assert.equal(await submit('中'.repeat(22)),false,'66 bytes exceed the limit');
 assert.equal(await submit('',true),true);assert.equal(posts[0].password,'');
}
for(const [code,text] of [['busy','设备正在处理，请稍后重试。'],['request_conflict','此操作编号已使用，请重试。'],['invalid_command','输入不符合要求，请检查后重试。'],['state_unavailable','设备状态暂时不可用，请稍后重试。'],['no_memory','设备资源不足，请稍后重试。']]){
 const s=state();const usb={async openSession(){return {session_id:'d'.repeat(32),remaining_seconds:90};},async stateGet(){return s;},jobProvesAdmission(){return false;},async command(){throw Object.assign(new Error(code),{code:'device_'+code});},async close(){}};
 const {context,node,test}=harness({usb:true,usbSession:usb});context.fetch=async path=>{if(path==='/api/state')return response({csrf_token:'synthetic-csrf',interfaces:[],device:{}});throw Error('unexpected host API');};
 await test.connectUsbPort();await test.connectUsbSession();await test.command('refresh',{},'');assert.equal(node('message').textContent,text,`device_${code} uses the shared message table`);assert.equal(test.hasSession(),true,'a device rejection keeps the session');
}
for(const code of ['serial_closed','serial_read_error','serial_write_timeout']){
 let closes=0;const s=state();const usb={async openSession(){return {session_id:'e'.repeat(32),remaining_seconds:90};},async stateGet(){return s;},jobProvesAdmission(){return false;},async command(){throw Object.assign(new Error(code),{code});},async close(){closes++;}};
 const {context,node,test}=harness({usb:true,usbSession:usb});context.fetch=async path=>{if(path==='/api/state')return response({csrf_token:'synthetic-csrf',interfaces:[],device:{}});throw Error('unexpected host API');};
 await test.connectUsbPort();await test.connectUsbSession();assert.equal(node('usb-open').hidden,true);await test.command('refresh',{},'');await new Promise(resolve=>setImmediate(resolve));
 assert.equal(closes,1,`${code} releases the dead serial link`);assert.equal(test.hasSession(),false);assert.equal(node('usb-open').hidden,false,'the Connect button reappears');
}
{
 /* Real serial layer: onTransmit fires only after the frame is written. */
 assert(deviceSerialMessage(Object.assign(new Error('busy'),{code:'device_session_busy'})).includes('数秒'));
 const makePort=({failWrite=false}={})=>{let controller;const writes=[];const readable=new ReadableStream({start(c){controller=c;}});const writable=new WritableStream({write(chunk){if(failWrite)throw Error('write failed');writes.push(chunk);const id=JSON.parse(new TextDecoder().decode(chunk).slice(5)).request_id;controller.enqueue(new TextEncoder().encode(`@AIQ:${JSON.stringify({v:2,op:'result',request_id:id,ok:true})}\n`));}});return {port:{readable,writable},writes};};
 const frame=new TextEncoder().encode('@AIQ:'+JSON.stringify({v:2,op:'state_get',request_id:'0a0b0c0d'})+'\n');
 let fired=0;const ok=makePort();const serial=startDeviceSerial(ok.port,'0a0b0c0d');
 const result=await serial.send(frame,{timeoutMs:1000,bootWaitMs:0,onWritten(){fired++;assert.equal(ok.writes.length,1,'frame is already written');}});assert.equal(result.ok,true);assert.equal(fired,1);await serial.close();
 const bad=makePort({failWrite:true});const failing=startDeviceSerial(bad.port,'0a0b0c0d');fired=0;
 await assert.rejects(failing.send(frame,{timeoutMs:1000,bootWaitMs:0,onWritten(){fired++;}}),{code:'serial_write_error'});assert.equal(fired,0,'a failed write is not reported as transmitted');await failing.close();
 const session=new UsbDeviceSession({async send(_bytes,_id,options){options.onWritten?.();return {ok:false,error_code:'busy'};},async close(){}},{requestId:()=>'11223344'});
 session.sessionId='f'.repeat(32);session.limits={max_command_bytes:2048,max_frame_bytes:4096,max_state_bytes:16384};session.sessionExpiresAt=Date.now()+60000;
 const refresh=(id,onTransmit)=>({op:'command',request_id:id,body:{v:1,op:'refresh',request_id:id},onTransmit});let transmitted=0;await assert.rejects(session.mutation(refresh('55667788',()=>{transmitted++;})),{code:'device_busy'});assert.equal(transmitted,1);
 const timeoutThenBusy=new UsbDeviceSession({calls:0,async send(_bytes,_id,options){options.onWritten?.();if(this.calls++===0)throw Object.assign(new Error('timeout'),{code:'serial_timeout'});return {ok:false,error_code:'busy'};},async close(){}},{requestId:()=>'11223344'});
 timeoutThenBusy.sessionId='f'.repeat(32);timeoutThenBusy.limits=session.limits;timeoutThenBusy.sessionExpiresAt=Date.now()+60000;
 await assert.rejects(timeoutThenBusy.mutation(refresh('55667788')),error=>error.code==='device_busy'&&error.diagnostics.retry_used===true);
 assert.equal(session.sessionId.length,32);await assert.rejects(session.mutation(refresh('55667788')),error=>error.code==='device_busy'&&error.diagnostics.retry_used===false);
 session.sessionExpiresAt=Date.now()-1;transmitted=0;await assert.rejects(session.mutation(refresh('55667788',()=>{transmitted++;})),{code:'device_session_expired'});assert.equal(transmitted,0,'a locally detected expiry never reaches the device');
}
{
 /* A hidden USB page keeps polling so the device never sees its session as idle; the superseded page learns why it was cut off. */
 const s=state();let reads=0,throwInvalid=false;const usb={async openSession(){return {session_id:'a1'.repeat(16),remaining_seconds:90};},async stateGet(){reads++;if(throwInvalid)throw Object.assign(new Error('invalid'),{code:'device_invalid_session'});return s;},jobProvesAdmission(){return false;},async close(){}};
 const {context,document,node,test,tickIntervals}=harness({usb:true,usbSession:usb});context.fetch=async path=>{if(path==='/api/state')return response({csrf_token:'synthetic-csrf',interfaces:[],device:{}});throw Error('unexpected host API');};
 await test.connectUsbPort();await test.connectUsbSession();const before=reads;document.hidden=true;await tickIntervals();assert.equal(reads,before+1,'a hidden page with an open USB session still reads state');
 throwInvalid=true;await tickIntervals();assert.equal(test.hasSession(),false);assert(deviceSerialMessage(Object.assign(new Error('x'),{code:'device_invalid_session'})).includes('另一个页面'));
 document.hidden=true;const idle=reads;await tickIntervals();assert.equal(reads,idle,'no session, no hidden polling');
}
{
 const {context,document,test,tickIntervals}=harness(),s=state();let fetches=0;test.setup(s);context.fetch=async()=>{fetches++;return response(s);};document.hidden=true;await tickIntervals();assert.equal(fetches,0,'hidden AP pages stay quiet');
}
console.log('Shared device page: AP regression and USB-native settings, secret routing, fixed Codex URL, no network request from USB settings PASS');

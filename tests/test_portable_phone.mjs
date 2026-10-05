// Execute the production shared setup page against a synthetic in-memory DOM.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';
const html=fs.readFileSync(new URL('../main/portable_setup.html',import.meta.url),'utf8');
const source=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const secret='s'.repeat(43);
new vm.Script(source);
assert(!/localStorage|sessionStorage|indexedDB/.test(source),'secrets have no browser storage path');
assert(!/mode_select|id="mode"|switch-direct/.test(html),'shared setup has no global source selector');
function harness(){
 const nodes=new Map();
 const node=id=>{if(!nodes.has(id))nodes.set(id,{id,value:'',innerHTML:'',textContent:'',className:'',hidden:false,disabled:false,checked:false,dataset:{},classList:{toggle(){}},setAttribute(){},querySelectorAll(){return [];},querySelector(){return node(id+'-button');}});return nodes.get(id);};
 for(const match of html.matchAll(/id="([^"]+)"/g))node(match[1]);
 const tabs=['accounts','network','settings'].map(tab=>({dataset:{tab},classList:{toggle(){}},setAttribute(){}}));
 const document={hidden:false,getElementById:node,querySelectorAll:selector=>selector==='[data-tab]'?tabs:selector==='button,input,select'?[...nodes.values()]:[],addEventListener(){}};
 const context={document,location:{hash:'',pathname:'/'},history:{replaceState(_a,_b,path){context.cleaned=path;}},URLSearchParams,TextEncoder,Date,crypto:webcrypto,setInterval(){},setTimeout:callback=>callback(),fetch:async()=>{throw Error('unexpected fetch');}};
 vm.createContext(context);vm.runInContext(source+`\nglobalThis.test={render,jobText,accountStatus,command,poll,launchCodex,startSession,setup(stateValue){state=stateValue;stopped=false;busy=false;launchBusy=false;setupSecret='${secret}';},select(id){selectedId=id;confirmation=null;removingId='';},getSecret(){return setupSecret;}};`,context);
 return {context,node,test:context.test};
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
console.log('Shared device page: manual secret, mixed sources, lossless swap/rebind/import, pending network, scoped errors, authorization sequencing and AP cutover PASS');

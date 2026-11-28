'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const source = fs.readFileSync(path.join(root, 'assets/webview-auth-extension/worker.js'), 'utf8');

// Only the browser event/target lookup and native binding are adapters. Every
// dispatch, pending callback, epoch and metadata decision executes the real worker.
function fixture() {
  const handlers = {}, registrations = {}, emitted = [], queued = [];
  let defer = false;
  const targets = [{id:'target-A',tabId:10,url:'https://same.invalid/file'},
    {id:'target-B',tabId:20,url:'https://same.invalid/file'}];
  const hook = name => ({addListener(callback, filter, extra) {
    handlers[name] = callback; registrations[name] = {filter,extra};
  }});
  const chrome = {runtime:{},debugger:{getTargets(callback) {
    if(defer) queued.push(callback); else callback(targets);
  }},webRequest:{onBeforeRequest:hook('before'),onAuthRequired:hook('auth'),onCompleted:hook('completed'),onErrorOccurred:hook('error')}};
  const context = vm.createContext({chrome,gesherAuth_fixture:message=>emitted.push(JSON.parse(message))});
  vm.runInContext(source,context,{filename:'production-worker.js'});
  const dispatch = command => context.gesherAuthDispatch({channel:'fixture-channel',
    ...(command.op==='cancelTarget'?{generation:'1'}:{}),
    ...(command.op==='hello'?{binding:'gesherAuth_fixture'}:{}),...command});
  const before = (tabId,requestId,extra={})=>handlers.before(Object.defineProperties({tabId,requestId,url:'https://same.invalid/file'},Object.getOwnPropertyDescriptors(extra)));
  const auth = (tabId, requestId='request-'+tabId, extra={}, begin=true) => {
    const calls=[];
    const details={tabId,requestId,url:'https://same.invalid/file',scheme:'basic',realm:'same-realm',
      challenger:{host:'same.invalid',port:443}};
    Object.defineProperties(details,Object.getOwnPropertyDescriptors(extra));
    if(begin)before(tabId,requestId,extra);
    handlers.auth(details,response=>calls.push(JSON.parse(JSON.stringify(response))));
    return calls;
  };
  const start = (epoch='epoch-1') => {
    assert.equal(dispatch({op:'hello',epoch}),true);
    for(const targetId of ['target-A','target-B']) assert.equal(dispatch({op:'register',epoch,targetId}),true);
  };
  return {handlers,registrations,emitted,queued,targets,dispatch,auth,before,start,
    binding(name){context[name]=message=>emitted.push(JSON.parse(message));},
    defer(value=true){defer=value;},flush(){assert.ok(queued.length);queued.shift()(targets);}};
}
const response=(epoch,targetId,ticket,more={})=>({op:'respond',epoch,targetId,ticket,...more});

test('packaged worker identity is stable and authentication observes no body API',()=>{
 const manifest=JSON.parse(fs.readFileSync(path.join(root,'assets/webview-auth-extension/manifest.json'),'utf8'));
 const digest=crypto.createHash('sha256').update(Buffer.from(manifest.key,'base64')).digest().subarray(0,16);
 const id=[...digest].map(byte=>String.fromCharCode(97+(byte>>4),97+(byte&15))).join('');
 assert.equal(id,'njmgmkjdhpmipobhjndpkbpegchdmpge');
 assert.equal(manifest.background.service_worker,'worker.js');
 assert.deepEqual([...manifest.permissions].sort(),['debugger','webRequest','webRequestAuthProvider'].sort());
 assert.equal('content_scripts' in manifest,false);
});

test('same URL in two views retains independent target, ticket and consent',()=>{
 const f=fixture();f.start();const a=f.auth(10,'same-request-a'),b=f.auth(20,'same-request-b');
 const [first,second]=f.emitted.filter(e=>e.event==='auth');
 assert.notEqual(first.ticket,second.ticket);assert.equal(first.targetId,'target-A');assert.equal(second.targetId,'target-B');
 assert.equal(f.dispatch(response('epoch-1','target-B',first.ticket,{proceed:true,user:'wrong-owner',password:'wrong'})),false);
 assert.deepEqual(a,[]);assert.deepEqual(b,[]);
 assert.equal(f.dispatch(response('epoch-1','target-B',second.ticket,{proceed:false})),true);
 assert.deepEqual(b,[{cancel:true}]);assert.deepEqual(a,[]);
 assert.equal(f.dispatch(response('epoch-1','target-A',first.ticket,{proceed:true,user:'owner',password:'secret'})),true);
 assert.deepEqual(a,[{authCredentials:{username:'owner',password:'secret'}}]);
 assert.equal(f.dispatch(response('epoch-1','target-A',first.ticket,{proceed:true,user:'late',password:'late'})),false);
});

test('POST bodies and credentials never enter the host metadata binding',()=>{
 const f=fixture();f.start();let bodyReads=0;
 const details={method:'POST',requestBody:{bytes:'x'.repeat(2<<20)}};
 Object.defineProperty(details,'postData',{enumerable:true,get(){bodyReads++;throw Error('postData read');}});
 // Request metadata provided by Chrome can contain unrelated fields; the worker
 // must select its own metadata fields instead of serializing the input object.
 const calls=f.auth(10,'large-post',details);
 const event=f.emitted.find(e=>e.event==='auth');
 assert.equal(event.requestId,'large-post');assert.ok(JSON.stringify(event).length<1024);
 for(const forbidden of ['requestBody','postData','headers','password','user','authCredentials']) assert.equal(forbidden in event,false);
 assert.deepEqual(calls,[]);assert.equal(bodyReads,0);
 assert.deepEqual(Array.from(f.registrations.auth.extra),['asyncBlocking']);
 assert.deepEqual(Object.keys(f.handlers).sort(),['auth','before','completed','error']);
 assert.equal(f.registrations.before.extra,undefined,'Readonly request-start observation requested body access');
});

test('Stop cancels only its exact target, rejects late replies, and permits later authentication',()=>{
 const f=fixture();f.start();const a=f.auth(10,'a'),b=f.auth(20,'b');
 const ticket=f.emitted.find(e=>e.event==='auth'&&e.targetId==='target-A').ticket;
 assert.equal(f.dispatch({op:'cancelTarget',epoch:'epoch-1',targetId:'target-A'}),true);
 assert.deepEqual(a,[{cancel:true}]);assert.deepEqual(b,[]);
 assert.equal(f.dispatch(response('epoch-1','target-A',ticket,{proceed:true,user:'late',password:'late'})),false);
 const later=f.auth(10,'later');assert.deepEqual(later,[]);
 assert.equal(f.emitted.filter(e=>e.event==='auth'&&e.targetId==='target-A').length,2);
});

test('destroy/unregister and shutdown settle once and cannot accept stale credentials',()=>{
 const f=fixture();f.start();const a=f.auth(10),b=f.auth(20);
 const old=f.emitted.filter(e=>e.event==='auth');
 f.dispatch({op:'unregister',epoch:'epoch-1',targetId:'target-A'});
 assert.deepEqual(a,[{cancel:true}]);assert.deepEqual(f.auth(10,'no-owner'),[{}]);
 f.dispatch({op:'shutdown',epoch:'epoch-1'});assert.deepEqual(b,[{cancel:true}]);
 assert.equal(f.dispatch(response('epoch-1','target-B',old[1].ticket,{proceed:true,user:'late',password:'late'})),false);
 assert.deepEqual(f.auth(20,'after-shutdown'),[{}]);
});

test('completed/failed request retires only that exact browser request ID',()=>{
 for(const ended of ['completed','error']){
  const f=fixture();f.start();const a=f.auth(10,'a'),b=f.auth(20,'b');
  const event=f.emitted.find(e=>e.event==='auth'&&e.requestId==='a');
  f.handlers[ended]({requestId:'a'});assert.deepEqual(a,[]);assert.deepEqual(b,[]);
  assert.equal(f.dispatch(response('epoch-1','target-A',event.ticket,{proceed:true,user:'late',password:'late'})),false);
  assert.equal(f.emitted.filter(e=>e.event==='ended'&&e.targetId==='target-A').length,1);
 }
});

test('epoch replacement cancels pending prompts and rejects old process replies',()=>{
 const f=fixture();f.start();const calls=f.auth(10);const old=f.emitted.find(e=>e.event==='auth');
 f.dispatch({op:'hello',epoch:'epoch-2'});assert.deepEqual(calls,[{cancel:true}]);
 assert.equal(f.dispatch(response('epoch-1','target-A',old.ticket,{proceed:true,user:'stale',password:'stale'})),false);
 assert.deepEqual(f.auth(10,'unregistered-new-epoch'),[{}]);
});

test('queued hello lookup cannot emit readiness for a newer worker epoch',()=>{
 const f=fixture();f.defer();f.dispatch({op:'hello',epoch:'old'});f.dispatch({op:'hello',epoch:'new'});
 f.flush();assert.deepEqual(f.emitted,[]);
 f.flush();assert.deepEqual(f.emitted.map(e=>[e.event,e.epoch]),[['ready','new']]);
});

test('queued registration cannot resurrect an unregistered or shut-down view',()=>{
 for(const op of ['unregister','shutdown']){
  const f=fixture();f.dispatch({op:'hello',epoch:'epoch-1'});f.defer();
  f.dispatch({op:'register',epoch:'epoch-1',targetId:'target-A'});
  f.dispatch({op,epoch:'epoch-1',targetId:'target-A'});f.flush();
  assert.equal(f.emitted.some(e=>e.event==='registered'),false,op+' emitted a stale registration');
  assert.deepEqual(f.auth(10,'late-browser-event'),[{}]);
 }
});

test('queued registration from an older epoch never enters a new epoch',()=>{
 const f=fixture();f.dispatch({op:'hello',epoch:'old'});f.defer();
 f.dispatch({op:'register',epoch:'old',targetId:'target-A'});f.dispatch({op:'hello',epoch:'new'});
 f.flush();assert.equal(f.emitted.some(e=>e.event==='registered'),false);f.flush();
 assert.deepEqual(f.auth(10,'new-epoch'),[{}]);
});

test('capacity rejection is bounded and later pending slots remain usable',()=>{
 const f=fixture();f.start();const held=[];
 for(let i=0;i<64;i++)held.push(f.auth(10,'pending-'+i));
 assert.deepEqual(f.auth(10,'overflow'),[{cancel:true}]);
 assert.equal(f.emitted.filter(e=>e.event==='capacity').length,1);
 f.handlers.error({requestId:'pending-0'});
 assert.deepEqual(f.auth(10,'recovered'),[]);
 assert.equal(f.emitted.filter(e=>e.event==='auth').length,65);
});


test('independent process channels cannot answer, cancel or reset another owner',()=>{
 const f=fixture();f.dispatch({op:'hello',epoch:'one'});f.dispatch({op:'register',epoch:'one',targetId:'target-A'});
 f.binding('gesherAuth_other');
 const other=command=>f.dispatch({channel:'other-channel',binding:'gesherAuth_other',epoch:'two',...command});
 other({op:'hello'});other({op:'register',targetId:'target-B'});
 const a=f.auth(10,'same-a'),b=f.auth(20,'same-b');
 const eventA=f.emitted.find(e=>e.event==='auth'&&e.targetId==='target-A');
 const eventB=f.emitted.find(e=>e.event==='auth'&&e.targetId==='target-B');
 assert.equal(eventA.channel,'fixture-channel');assert.equal(eventB.channel,'other-channel');
 assert.equal(other({op:'respond',targetId:'target-A',ticket:eventA.ticket,proceed:true,user:'foreign',password:'foreign'}),false);
 f.dispatch({op:'hello',epoch:'one-restarted'});assert.deepEqual(a,[{cancel:true}]);assert.deepEqual(b,[]);
 f.dispatch({op:'shutdown',epoch:'one-restarted'});assert.deepEqual(b,[]);
 assert.equal(other({op:'respond',targetId:'target-B',ticket:eventB.ticket,proceed:true,user:'owner-B',password:'secret-B'}),true);
 assert.deepEqual(b,[{authCredentials:{username:'owner-B',password:'secret-B'}}]);
});

test('a second process cannot take an already registered native target',()=>{
 const f=fixture();f.dispatch({op:'hello',epoch:'one'});f.dispatch({op:'register',epoch:'one',targetId:'target-A'});
 f.binding('gesherAuth_other');
 f.dispatch({op:'hello',channel:'other-channel',binding:'gesherAuth_other',epoch:'two'});
 f.dispatch({op:'register',channel:'other-channel',epoch:'two',targetId:'target-A'});
 assert.equal(f.emitted.filter(e=>e.event==='registrationFailed'&&e.channel==='other-channel').length,1);
 f.auth(10,'still-owner-A');
 assert.equal(f.emitted.filter(e=>e.event==='auth').at(-1).channel,'fixture-channel');
});

test('queued lookup for a removed owner cannot enter its replacement owner',()=>{
 const f=fixture();f.dispatch({op:'hello',epoch:'old'});f.defer();
 f.dispatch({op:'register',epoch:'old',targetId:'target-A'});
 f.dispatch({op:'shutdown',epoch:'old'});f.binding('gesherAuth_fixture');f.dispatch({op:'hello',epoch:'replacement'});
 f.flush();assert.equal(f.emitted.some(e=>e.event==='registered'),false);
 f.flush();f.dispatch({op:'register',epoch:'replacement',targetId:'target-A'});f.flush();
 f.defer(false);f.auth(10,'replacement-request');
 const auth=f.emitted.find(e=>e.event==='auth');assert.equal(auth.epoch,'replacement');
});


test('request-start generation rejects old challenges after Stop but permits new requests',()=>{
 const f=fixture();f.start();f.before(10,'old-request');
 f.dispatch({op:'cancelTarget',epoch:'epoch-1',targetId:'target-A',generation:'1'});
 assert.deepEqual(f.auth(10,'old-request',{},false),[{cancel:true}]);
 assert.equal(f.emitted.filter(e=>e.event==='auth').length,0);
 assert.deepEqual(f.auth(10,'new-request'),[]);
 assert.equal(f.emitted.filter(e=>e.event==='auth').at(-1).generation,'1');
});

test('auth target lookup completing after Stop cannot revive consent',()=>{
 const f=fixture();f.start();f.defer();const pending=f.auth(10,'lookup-pending');
 f.dispatch({op:'cancelTarget',epoch:'epoch-1',targetId:'target-A',generation:'1'});
 assert.deepEqual(pending,[{cancel:true}]);f.flush();
 assert.equal(f.emitted.filter(e=>e.event==='auth').length,0);
});

test('browser tab reuse is revalidated against exact native target before consent',()=>{
 const f=fixture();f.start();f.targets[0]={id:'replacement-target',tabId:10,url:'https://same.invalid/file'};
 assert.deepEqual(f.auth(10,'reused-tab'),[{cancel:true}]);
 assert.equal(f.emitted.filter(e=>e.event==='auth').length,0);
});


test('repeated last-owner shutdown releases channel and native binding capacity',()=>{
 const f=fixture();
 for(let i=0;i<270;i++){
  const channel='channel-'+i,binding='gesherAuth_owner'+i,epoch='epoch-'+i;
  f.binding(binding);
  assert.equal(f.dispatch({op:'hello',channel,binding,epoch}),true);
  assert.equal(f.dispatch({op:'register',channel,epoch,targetId:'target-A'}),true);
  f.auth(10,'owner-request-'+i);
  const event=f.emitted.filter(e=>e.event==='auth').at(-1);
  assert.equal(event.channel,channel);
  assert.equal(f.dispatch({op:'shutdown',channel,epoch}),true);
 }
});

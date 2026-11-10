#include "link_page.h"

namespace ls {

const char* const kLinkPageHeaders =
    "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
    "connect-src 'self'; img-src data:; frame-src 'self'; worker-src 'self'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'none'\r\n"
    "X-Frame-Options: DENY\r\n";

static const char* kPageA = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex"><meta name="referrer" content="no-referrer">
<title>LunaShare</title>
<style>
:root{--bg:#f4f5f8;--card:#fff;--fg:#1d2230;--mut:#6a7183;--acc:#5b6cf0;--ok:#1f9d55;--bad:#d64545;--line:#e3e6ee}
@media(prefers-color-scheme:dark){:root{--bg:#14161c;--card:#1d2029;--fg:#e8eaf0;--mut:#9aa1b2;--acc:#7d8bff;--line:#2c303c}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;display:flex;min-height:100vh;align-items:center;justify-content:center;padding:16px}
.c{background:var(--card);border:1px solid var(--line);border-radius:16px;max-width:460px;width:100%;padding:24px;box-shadow:0 6px 24px rgba(0,0,0,.08)}
.brand{font-weight:600;color:var(--acc);font-size:14px;letter-spacing:.3px}
h1{font-size:20px;margin:10px 0 4px;word-break:break-word}.mut{color:var(--mut);font-size:14px}
.sha{font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;color:var(--mut);word-break:break-all;margin-top:12px}
button{background:var(--acc);color:#fff;border:0;border-radius:10px;padding:12px 18px;font-size:16px;width:100%;margin-top:18px;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
input{width:100%;font-size:18px;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--fg);margin-top:14px;letter-spacing:2px}
.bar{height:8px;background:var(--line);border-radius:4px;margin-top:18px;overflow:hidden;display:none}.bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
.msg{margin-top:14px;font-size:15px}.ok{color:var(--ok)}.bad{color:var(--bad)}.hide{display:none}
</style></head><body><div class="c">
<div class="brand">LunaShare</div>
<h1 id="name">&nbsp;</h1><div class="mut" id="size"></div>
<input id="pin" class="hide" inputmode="numeric" autocomplete="off">
<button id="go" disabled></button>
<div class="bar" id="bar"><i id="fill"></i></div>
<div class="msg" id="msg"></div>
<div class="sha" id="sha"></div>
</div>
<script id="cfg" type="application/json">)HTML";

static const char* kPageB = R"HTML(</script>
<script>
(function(){
"use strict";
var cfg=JSON.parse(document.getElementById('cfg').textContent);
var pl=/^pl\b/i.test(navigator.language||'');
var T=pl?{dl:'Pobierz',again:'Pobierz ponownie',pin:'Kod PIN',pinGo:'Dalej',wrong:'Zły PIN. Pozostało prób: ',wait:'Czekam, aż nadawca pozwoli…',gone:'Ten link wygasł lub został wyłączony.',used:'Ten link został już wykorzystany.',changed:'Plik zmienił się od wysłania linku - poproś o nowy.',refused:'Nadawca nie zgodził się.',preparing:'Nadawca przygotowuje plik…',offline:'Urządzenie nadawcy jest teraz offline. Spróbuję ponownie…',net:'Problem z połączeniem. Ponawiam…',fail:'Nie udało się pobrać pliku.',verified:'✓ Pobrano i sprawdzono - każda część jest dokładnie taka, jak u nadawcy.',hashOk:'✓ Suma SHA-256 zgodna.',saving:'Zapisywanie…',started:'Pobieranie rozpoczęte - zobacz pobrane pliki w przeglądarce.',nokey:'Link jest niepełny - skopiuj go w całości (razem z częścią po #).',https:'Ta strona wymaga połączenia HTTPS.',tamper:'Część pliku nie przeszła kontroli. Ponawiam…',from:'Plik od użytkownika LunaShare',sha:'SHA-256: '}
:{dl:'Download',again:'Download again',pin:'PIN',pinGo:'Continue',wrong:'Wrong PIN. Tries left: ',wait:'Waiting for the sender to allow it…',gone:'This link has expired or was turned off.',used:'This link has already been used.',changed:'The file changed since the link was sent - ask for a new one.',refused:'The sender said no.',preparing:'The sender is preparing the file…',offline:"The sender's device is offline right now. Trying again…",net:'Connection problem. Retrying…',fail:"Couldn't download the file.",verified:'✓ Downloaded and checked - every part is exactly what the sender has.',hashOk:'✓ SHA-256 matches.',saving:'Saving…',started:"The download has started - see your browser's downloads.",nokey:'The link is incomplete - copy all of it (including the part after #).',https:'This page needs an HTTPS connection.',tamper:"A part of the file didn't pass the check. Retrying…",from:'A file from a LunaShare user',sha:'SHA-256: '};
var $=function(i){return document.getElementById(i)};
var go=$('go'),msg=$('msg'),pinEl=$('pin'),bar=$('bar'),fill=$('fill');
pinEl.placeholder=T.pin;go.textContent=T.dl;
function say(t,cls){msg.textContent=t||'';msg.className='msg'+(cls?' '+cls:'')}
function fmt(n){var u=['B','KB','MB','GB','TB'],i=0;while(n>=1024&&i<4){n/=1024;i++}return (i?n.toFixed(n<10?1:0):n)+' '+u[i]}
function rnd(n){var b=new Uint8Array(n);crypto.getRandomValues(b);return b64(b)}
function b64(b){var s='';for(var i=0;i<b.length;i++)s+=String.fromCharCode(b[i]);return btoa(s).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'')}
function unb64(s){s=s.replace(/-/g,'+').replace(/_/g,'/');while(s.length%4)s+='=';var r=atob(s),b=new Uint8Array(r.length);for(var i=0;i<r.length;i++)b[i]=r.charCodeAt(i);return b}
function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}
var key='ls-sid-'+location.pathname;var sid=null;
try{sid=sessionStorage.getItem(key)}catch(e){}
if(!sid){sid=rnd(16);try{sessionStorage.setItem(key,sid)}catch(e){}}
var info=null,pinVal='',ended=false;
function stop(st){ended=true;go.disabled=true;go.classList.add('hide');pinEl.classList.add('hide');say(T[st]||T.fail,'bad')}
function showInfo(i){info=i;$('name').textContent=i.name;$('size').textContent=fmt(i.size);if(i.sha)$('sha').textContent=T.sha+i.sha}
function askPin(left,wrong){pinEl.classList.remove('hide');go.textContent=T.pinGo;go.disabled=false;pinEl.focus();if(wrong)say(T.wrong+left,'bad');else say('')}
function handle(j){ // a status reply: true = go on
  var st=j.st;
  if(st==='ok')return true;
  if(st==='pin'){askPin(j.left,j.wrong);return false}
  if(st==='wait'||st==='preparing'){say(T[st]);setTimeout(start,2000);return false}
  if(st==='offline'){say(T.offline);setTimeout(start,4000);return false}
  stop(st);return false;
}

// ---- served by the device on this network ----
function lanStart(){
  var u=location.pathname+'/start?sid='+encodeURIComponent(sid)+(pinVal?'&pin='+encodeURIComponent(pinVal):'');
  fetch(u,{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
    if(!handle(j))return;
    pinEl.classList.add('hide');showInfo({name:j.name,size:j.size,sha:j.sha});
    go.textContent=T.dl;go.disabled=false;say('');ready=true;
  }).catch(function(){say(T.net);setTimeout(start,3000)});
}
function lanDownload(){
  location.href=location.pathname+'/file?sid='+encodeURIComponent(sid);
  say(T.started,'ok');go.textContent=T.again;
}

// ---- through the server, end-to-end encrypted ----
var sub=window.crypto&&crypto.subtle,K=null,enc=new TextEncoder();
var id=location.pathname.split('/')[2]||'';
async function keys(){
  var S=unb64(location.hash.slice(1));
  if(S.length!==16)throw 'nokey';
  var hk=await sub.importKey('raw',S,{name:'HMAC',hash:'SHA-512'},false,['sign']);
  var ek=new Uint8Array(await sub.sign('HMAC',hk,enc.encode('LunaShare link enc'))).slice(0,32);
  var mk=new Uint8Array(await sub.sign('HMAC',hk,enc.encode('LunaShare link mac'))).slice(0,32);
  return {aes:await sub.importKey('raw',ek,'AES-CTR',false,['decrypt']),mac:await sub.importKey('raw',mk,{name:'HMAC',hash:'SHA-512'},false,['sign'])};
}
async function open(buf,idx,last){
  var n=buf.length-32;if(n<0)return null;
  var m=new Uint8Array(13+n);m.set(enc.encode('LSL1'));new DataView(m.buffer).setBigUint64(4,idx);m[12]=last?1:0;m.set(buf.subarray(0,n),13);
  var t=new Uint8Array(await sub.sign('HMAC',K.mac,m)),d=0;
  for(var i=0;i<32;i++)d|=t[i]^buf[n+i];
  if(d)return null;
  var ctr=new Uint8Array(16);new DataView(ctr.buffer).setBigUint64(0,idx);
  return new Uint8Array(await sub.decrypt({name:'AES-CTR',counter:ctr,length:64},K.aes,buf.subarray(0,n)));
}
async function pinProof(p){
  var t=new Uint8Array(await sub.sign('HMAC',K.mac,enc.encode('pin:'+p))).slice(0,16),s='';
  for(var i=0;i<16;i++)s+=(t[i]<16?'0':'')+t[i].toString(16);return s;
}
async function webStart(){
  try{
    if(!sub)return stop('https');
    if(!K)K=await keys();
    var u='/api/'+id+'/meta?sid='+encodeURIComponent(sid)+(pinVal?'&pp='+await pinProof(pinVal):'');
    var r=await fetch(u,{cache:'no-store'});
    if((r.headers.get('content-type')||'').indexOf('json')>=0){handle(await r.json());return}
    if(!r.ok)throw 'net';
    var m=await open(new Uint8Array(await r.arrayBuffer()),0xFFFFFFFFFFFFFFFFn,true);
    if(!m)return stop('tamper');
    var j=JSON.parse(new TextDecoder().decode(m));
    pinEl.classList.add('hide');showInfo({name:j.n,size:j.s,sha:j.h,chunk:j.c});
    go.textContent=T.dl;go.disabled=false;say('');ready=true;
  }catch(e){
    if(e==='nokey')return stop('nokey');
    say(T.net);setTimeout(start,3000);
  }
}
async function part(k,last){ // fetched, checked and decrypted; retries until it works or the link ends
  for(var tries=0;;tries++){
    try{
      var r=await fetch('/api/'+id+'/p/'+k+'?sid='+encodeURIComponent(sid),{cache:'no-store'});
      if((r.headers.get('content-type')||'').indexOf('json')>=0){
        var j=await r.json();
        if(j.st==='offline'){say(T.offline);await sleep(4000);continue}
        throw {end:j.st};
      }
      if(!r.ok)throw 'net';
      var p=await open(new Uint8Array(await r.arrayBuffer()),BigInt(k),last);
      if(p)return p;
      say(T.tamper,'bad');
    }catch(e){
      if(e&&e.end)throw e;
      if(tries>30)throw {end:'fail'};
      say(T.net);
    }
    await sleep(Math.min(8000,500*(tries+1)));
  }
}
var big=(+new URLSearchParams(location.search).get('mem')||256)*1024*1024; // (?mem=: tests of the streamed way)
async function webDownload(){
  go.disabled=true;bar.style.display='block';say('');
  var n=Math.max(1,Math.ceil(info.size/info.chunk)),done=0,next=0,want=0,got={},waiters={};
  var stream=info.size>big&&'serviceWorker' in navigator?await swSink(info):null;
  function prog(){fill.style.width=(n?100*done/n:100)+'%'}
  var failed=null;
  async function worker(){
    while(next<n&&!failed){
      while(next-want>=8&&!failed)await sleep(50); // don't run far ahead of what's been written
      var k=next++;
      try{var p=await part(k,k===n-1)}catch(e){failed=e.end||'fail';break}
      got[k]=p;done++;prog();if(msg.className.indexOf('bad')>=0||msg.textContent===T.net||msg.textContent===T.offline)say('');
      if(waiters[k]){waiters[k]();delete waiters[k]}
    }
  }
  function take(k){return new Promise(function(res){if(got[k]||failed)return res();waiters[k]=res})}
  var ws=[worker(),worker(),worker()];
  var pieces=[],sha=info.size<=128*1024*1024&&!stream?[]:null;
  for(want=0;want<n;want++){
    await take(want);
    while(!got[want]&&!failed)await sleep(50);
    if(failed)break;
    var p=got[want];delete got[want];
    if(stream){try{await stream.write(p)}catch(e){failed='fail';break}}else pieces.push(p);
  }
  await Promise.all(ws);
  if(failed){if(stream)stream.abort();bar.style.display='none';return stop(failed)}
  if(stream){await stream.close();say(T.verified,'ok');go.textContent=T.again;go.disabled=false;return}
  var blob=new Blob(pieces,{type:'application/octet-stream'});pieces=null;
  var a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=info.name;document.body.appendChild(a);a.click();
  setTimeout(function(){a.remove()},1000);
  var line=T.verified;
  if(sha&&info.sha){
    try{var h=new Uint8Array(await sub.digest('SHA-256',await blob.arrayBuffer())),s='';for(var i=0;i<h.length;i++)s+=(h[i]<16?'0':'')+h[i].toString(16);
      if(s===info.sha)line+=' '+T.hashOk;else return stop('tamper')}catch(e){}
  }
  say(line,'ok');go.textContent=T.again;go.disabled=false;
}
async function swSink(i){ // a stream the service worker hands to the browser as a download
  try{
    var reg=await navigator.serviceWorker.register('/sw.js',{scope:'/s/'});
    var w=reg.active||reg.installing||reg.waiting;
    if(!reg.active)await new Promise(function(res){w.addEventListener('statechange',function(){if(w.state==='activated')res()});setTimeout(res,3000)});
    w=reg.active;if(!w)return null;
    var ch=new MessageChannel(),url='/s/dl/'+rnd(12),credits=0,wake=null,cancelled=false,ready=null;
    var isReady=new Promise(function(res){ready=res});
    ch.port1.onmessage=function(e){
      if(e.data==='ready')ready(true);
      if(e.data==='pull')credits++;
      if(e.data==='cancel')cancelled=true;
      if(wake){var f=wake;wake=null;f()}
    };
    w.postMessage({url:url,name:i.name,size:i.size,port:ch.port2},[ch.port2]);
    if(!await Promise.race([isReady,sleep(3000)]))return null;
    var f=document.createElement('iframe');f.hidden=true;f.src=url;document.body.appendChild(f);
    return {
      write:async function(p){
        while(credits<=0&&!cancelled)await new Promise(function(res){wake=res});
        if(cancelled)throw {end:'fail'};
        credits--;ch.port1.postMessage(p,[p.buffer]);
      },
      close:async function(){ch.port1.postMessage('end')},
      abort:function(){ch.port1.postMessage('abort')}
    };
  }catch(e){return null}
}

var ready=false;
function start(){if(ended)return;if(cfg.mode==='lan')lanStart();else webStart()}
go.onclick=function(){
  if(!pinEl.classList.contains('hide')){pinVal=pinEl.value.trim();if(!pinVal)return;go.disabled=true;start();return}
  if(!ready)return;
  if(cfg.mode==='lan')lanDownload();else webDownload();
};
pinEl.onkeydown=function(e){if(e.key==='Enter')go.onclick()};
if(cfg.st){stop(cfg.st);return}
$('name').textContent=cfg.name||T.from;if(cfg.size!==undefined)$('size').textContent=fmt(cfg.size);
if(cfg.mode==='lan'&&cfg.pin)askPin(5,false);else start();
})();
</script></body></html>
)HTML";

std::string LinkPageHtml(const std::string& cfgJson) {
    std::string cfg;
    cfg.reserve(cfgJson.size());
    for (char c : cfgJson) {  // never ends the script element early
        if (c == '<') cfg += "\\u003c";
        else cfg.push_back(c);
    }
    return std::string(kPageA) + cfg + kPageB;
}

const char* LinkServiceWorkerJs() {
    return R"JS(// LunaShare: hands a stream from the page to the browser as a download
var pending={};
self.addEventListener('install',function(){self.skipWaiting()});
self.addEventListener('activate',function(e){e.waitUntil(self.clients.claim())});
self.addEventListener('message',function(e){var d=e.data;if(d&&d.url&&d.port){pending[d.url]=d;d.port.postMessage('ready')}});
self.addEventListener('fetch',function(e){
  var u=new URL(e.request.url),d=pending[u.pathname];
  if(!d)return;
  delete pending[u.pathname];
  var port=d.port,got=null;
  var rs=new ReadableStream({
    start:function(c){port.onmessage=function(m){
      if(m.data==='end')c.close();
      else if(m.data==='abort')c.error('aborted');
      else c.enqueue(m.data instanceof Uint8Array?m.data:new Uint8Array(m.data));
      if(got){var g=got;got=null;g()}
    }},
    pull:function(){return new Promise(function(res){got=res;port.postMessage('pull')})},
    cancel:function(){port.postMessage('cancel')}
  },{highWaterMark:1});
  var name=encodeURIComponent(d.name).replace(/['()]/g,escape);
  e.respondWith(new Response(rs,{headers:{'Content-Type':'application/octet-stream','Content-Length':String(d.size),
    'Content-Disposition':"attachment; filename*=UTF-8''"+name,'X-Content-Type-Options':'nosniff'}}));
});
)JS";
}

}  // namespace ls

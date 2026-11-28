(function () {
"use strict";
var SILO=location.pathname.indexOf("/plugins/")===0, HOST=SILO?"Silo":"P2Play", base="/plugins/mediamesh", appPrefix="",session,busy=false,currentHash="",pageOffset=0,pageLimit=30,libraryRefreshTimer=null;
if(!SILO){var uiMarker=location.pathname.lastIndexOf("/ui");if(uiMarker>=0)appPrefix=location.pathname.slice(0,uiMarker);}
function appPath(path){return appPrefix+path;}
var locale={},language="en",theme="light",userKey="",authEpoch=0;
var $=function(id){return document.getElementById(id);};
function tr(value){return typeof value==="string"&&locale[value]?locale[value]:value;}
var statusTimer=null;
function status(message,bad,timeout){var target=$("status");if(statusTimer!==null)clearTimeout(statusTimer);target.textContent=tr(message);target.className=bad?"visible error":"visible";target.setAttribute("aria-hidden","false");statusTimer=setTimeout(function(){target.classList.remove("visible");target.setAttribute("aria-hidden","true");statusTimer=null;},Number.isFinite(timeout)?Math.max(500,timeout):(bad?6500:2800));}
async function request(url,body,headers,method){
 var requestAuthEpoch=authEpoch;
 var opts={method:method||(body===undefined?"GET":"POST"),credentials:"same-origin",headers:{Accept:"application/json"},cache:"no-store"};
 if(body!==undefined){if(body instanceof FormData)opts.body=body;else{opts.headers["Content-Type"]="application/json";opts.body=JSON.stringify(body);}}
 Object.keys(headers||{}).forEach(function(k){opts.headers[k]=headers[k];});
 var res=await fetch(url,opts),data;
 if(res.status===204)return {};
 try{data=await res.json();}catch(_){throw new Error(HOST+tr(" returned an unreadable response."));}
 if(res.status===401&&!SILO){if(requestAuthEpoch===authEpoch)showLogin();throw new Error("Log in to this P2Play node.");}
 if(!res.ok)throw new Error(data.error||tr("Request failed (")+res.status+").");
 return data;
}
function api(path,body,method){return request(base+path,body,session?{"X-MediaMesh-CSRF":session.csrf}:{},method);}
function core(path,body,method){return request(appPath("/api/v1"+path),body,{},method);}
function native(path,body,method){return request(appPath("/api/v1"+path),body,{},method);}
var host=SILO?{
 session:function(){return api("/api/session");},
 search:function(q,tag){return api("/api/search?q="+encodeURIComponent(q)+"&tag="+encodeURIComponent(tag));},
 lists:function(){return api("/api/lists");},discover:function(){return api("/api/discover");},
 subscribe:function(pubkey){return api("/api/subscribe",{pubkey:pubkey});},
 add:function(r){return api("/api/add",{hash:r.hash});},
 settings:function(body){return api("/api/settings",body);}
}:{
 session:async function(){var me=await native("/auth/me");return{rank:me.rank,csrf:"",can_admin:me.rank>=50,user:me.username};},
 search:async function(q,tag){var d=await core("/catalog/search?q="+encodeURIComponent(q)+"&tag="+encodeURIComponent(tag)+"&limit=50");return{results:(d.results||[]).map(function(r){return Object.assign({},r,{supported:true,tags:r.tags||[]});})};},
 lists:function(){return core("/lists");},discover:function(){return core("/lists/discover");},
 subscribe:function(pubkey){return core("/lists/"+encodeURIComponent(pubkey.toLowerCase()),{});},
 add:function(r){var link=r.torrs||(r.hash.length===64?"magnet:?xt=urn:btmh:1220"+r.hash:"magnet:?xt=urn:btih:"+r.hash);return native("/torrents",{link:link,title:r.title},"POST");}
};
function showLogin(){
 if(libraryRefreshTimer!==null){clearInterval(libraryRefreshTimer);libraryRefreshTimer=null;}
 authEpoch++;
 session=null;
 $("login-view").hidden=false;
 $("main-nav").hidden=true;
 $("account-menu-wrap").hidden=true;
 $("account-popover").hidden=true;
 $("account-menu-trigger").setAttribute("aria-expanded","false");
 $("add-open").hidden=true;
 $("settings-open").hidden=true;
 ["search","lists","library","server","users","settings","preferences","continue"].forEach(function(x){var n=$(x+"-view");if(n)n.hidden=true;});
 $("settings-modal").hidden=true;
 $("add-modal").hidden=true;
 syncModalState();
}
async function run(fn){if(busy)return;busy=true;document.body.setAttribute("aria-busy","true");try{await fn();}catch(e){status(e.message||"Request failed.",true);}finally{busy=false;document.body.removeAttribute("aria-busy");}}
function el(tag,value,cls){var n=document.createElement(tag);if(value!==undefined){n.textContent=tr(value);if(n.firstChild&&typeof value==="string")n.firstChild.__i18nSource=value;}if(cls)n.className=cls;return n;}
function buttonIcon(name){var paths={search:"M11 19a8 8 0 1 1 5.657-2.343L21 21",trash:"M3 6h18M8 6V4h8v2m3 0-1 14H6L5 6m4 4v7m6-7v7",play:"m8 5 12 7-12 7z",folder:"M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v10H3z",copy:"M8 8V5a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-3M5 8h9a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2z",refresh:"M20 11a8 8 0 1 0 2 5M20 4v7h-7",plus:"M12 5v14m-7-7h14",user:"M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2m8-10a4 4 0 1 0 0-8 4 4 0 0 0 0 8",key:"M21 2l-2 2m-7.5 7.5a5 5 0 1 1-7.07 7.07 5 5 0 0 1 7.07-7.07zm0 0L17 6l3 3-2 2",check:"m5 12 4 4L19 6",rotate:"M3 12a9 9 0 1 0 2.64-6.36L3 8m0-5v5h5",bolt:"m13 2-3 9h7l-6 11 2-9H6z",external:"M14 3h7v7m-1-6-9 9M5 5h5m-5 5v9h9v-5",shield:"M12 22s8-4 8-11V5l-8-3-8 3v6c0 7 8 11 8 11z"};var d=paths[name];if(!d)return null;var ns="http://www.w3.org/2000/svg",svg=document.createElementNS(ns,"svg"),p=document.createElementNS(ns,"path");svg.setAttribute("viewBox","0 0 24 24");svg.setAttribute("class","button-icon");svg.setAttribute("aria-hidden","true");svg.setAttribute("focusable","false");p.setAttribute("d",d);svg.append(p);return svg;}
function button(label,fn,cls,icon){var b=el("button",label,cls);b.type="button";var icons={"Search":"search","Remove":"trash","Delete":"trash","Play":"play","Open":"external","Details / files":"folder","Copy stream link":"copy","Copy torrent stream link":"copy","Refresh":"refresh","Refresh files":"refresh","Reload":"refresh","Subscribe":"plus","Request refresh":"refresh","Add user":"user","Reset API token":"key","Reset password":"key","Reset progress":"rotate","Reset all progress":"rotate","Reset folder":"rotate","Mark all watched":"check","Mark all unviewed":"check","Mark folder watched":"check","Wake torrent":"bolt","Set role":"shield"};var name=icon||icons[label]||(label.indexOf(tr("Add to "))===0?"plus":"");var svg=buttonIcon(name);if(svg)b.insertBefore(svg,b.firstChild);b.addEventListener("click",function(){run(function(){return fn(b);});});return b;}
function bytes(n){n=Number(n);if(!Number.isFinite(n)||n<0)return tr("Size unknown");if(n===0)return tr("0 B");var u=["B","KiB","MiB","GiB","TiB"],i=0;while(n>=1024&&i<u.length-1){n/=1024;i++;}return n.toFixed(i?1:0)+" "+u[i];}
function formatDuration(seconds){
 var value=Number(seconds);if(!Number.isFinite(value)||value<0)value=0;
 var total=Math.floor(value),hours=Math.floor(total/3600),minutes=Math.floor((total%3600)/60),secs=total%60,parts=[];
 if(hours)parts.push(hours+" "+tr("h"));if(minutes)parts.push(minutes+" "+tr("min"));
 if(secs||!parts.length)parts.push(secs+" "+tr("s"));
 return parts.join(" ");
}
var renderedResultSets={};
function renderResults(results,targetId,onPick){var resultId=targetId||"results";results=Array.isArray(results)?results.filter(function(r){return r&&typeof r==="object";}):[];renderedResultSets[resultId]={results:results,onPick:onPick};var t=$(resultId);t.replaceChildren();if(!results.length)t.append(el("p","No releases found.","empty"));results.forEach(function(r){var row=el("article",undefined,"row"),copy=el("div",undefined,"row-copy"),tags=Array.isArray(r.tags)?r.tags.filter(function(x){return typeof x==="string";}):[];copy.append(el("h3",r.title||r.hash||"Untitled"),el("p",bytes(r.size)+" · "+tr("Files: ")+(r.files||0)),el("p",tags.join(", "),"hint"),el("code",r.hash||""));var action=onPick?button("Select",function(){onPick(r);}):button(tr("Add to ")+HOST,async function(b){b.disabled=true;await host.add(r);b.textContent=tr("Added");status(tr("Added to ")+HOST+".");});row.append(copy,action);t.append(row);});}
function renderLists(target,lists,discovery){target.replaceChildren();lists=Array.isArray(lists)?lists.filter(function(l){return l&&typeof l==="object";}):[];if(!lists.length)target.append(el("p",tr(discovery?"No public lists available.":"No lists subscribed."),"empty"));lists.forEach(function(l){var row=el("article",undefined,"row"),copy=el("div",undefined,"row-copy");copy.append(el("h3",l.name||tr("Unnamed list")),el("p",l.description||tr("No description")),el("p",tr("Version ")+l.seq,"hint"),el("code",l.pubkey||""));row.append(copy);if(session&&session.can_admin)row.append(button(tr(discovery?"Subscribe":"Request refresh"),async function(){await host.subscribe(l.pubkey);status("Subscription requested.");},undefined,discovery?"plus":"refresh"));target.append(row);});}
async function loadLists(){var a=await host.lists();renderLists($("lists"),a&&a.lists,false);var b=await host.discover();renderLists($("discover"),b&&b.lists,true);}
var activeMainView="library";
function showView(view){
 $("login-view").hidden=true;
 $("main-nav").hidden=false;
 var main=view==="search"||view==="library"||view==="continue";
 if(main)activeMainView=view;
 ["library","continue","search"].forEach(function(x){var n=$(x+"-view");if(n)n.hidden=x!==view;});
 var pane=view==="preferences"?"preferences-view":view+"-view";
 ["preferences-view","lists-view","server-view","users-view","settings-view"].forEach(function(id){var n=$(id);if(n)n.hidden=main||id!==pane;});
 $("settings-modal").hidden=main;
 if(!main){document.querySelectorAll("[data-settings-view]").forEach(function(b){b.classList.toggle("active",b.dataset.settingsView===view);});}
 document.querySelectorAll("[data-view]").forEach(function(b){b.setAttribute("aria-pressed",String(b.dataset.view===view));});
 syncModalState();
}
document.querySelectorAll("[data-view]").forEach(function(b){b.addEventListener("click",function(){run(async function(){if(!session)return;var v=b.dataset.view;showView(v);if(v==="lists")await loadLists();else if(v==="library")await loadLibrary();else if(v==="continue")await loadContinue();else if(v==="server")await loadServer();else if(v==="users")await loadUsers();else if(v==="settings")await loadConnection();});});});
async function loadConnection(){var cfg=await host.settings();$("daemon-url").value=cfg.url;$("daemon-token").value="";$("clear-token").checked=false;$("token-state").textContent=tr(cfg.token_configured?"Token configured.":"No token configured.");}
function categoryOptions(values){var s=$("library-category"),old=s.value;s.replaceChildren();var safe=Array.isArray(values)?values.filter(function(x){return typeof x==="string"&&x.length>0;}):[];[["","All categories"],["none","Uncategorized"]].concat(safe.map(function(x){return[x,x];})).forEach(function(x){var o=el("option",x[1]);o.value=x[0];s.append(o);});if(Array.from(s.options).some(function(o){return o.value===old;}))s.value=old;}
function coverUrl(hash){return appPath("/api/v1/torrents/"+encodeURIComponent(hash)+"/cover");}
function coverThumb(item){var src=item.poster&&item.poster.indexOf(appPath("/api/v1/torrents/"))===0?item.poster:(item.cover?coverUrl(item.hash):"");if(!src)return el("div",undefined,"cover-placeholder");var img=el("img",undefined,"cover-thumb");img.src=src;img.alt="";img.loading="lazy";return img;}
function saveLibraryField(item,key,value,restore){
 value=String(value==null?"":value).trim();var previous=String(item[key]||"");
 if(value===previous){if(restore)restore(previous);return;}
 var patch={};patch[key]=value;
 native("/torrents/"+encodeURIComponent(item.hash),patch,"PUT").then(function(){
  item[key]=value;if(restore)restore(value);status("Changes saved.");
 }).catch(function(e){if(restore)restore(previous);status(e.message||"Could not save changes.",true);});
}
function editTorrentTitle(item,heading){
 var input=el("input",undefined,"title-editor");input.type="text";input.maxLength=512;input.value=item.title||"";
 input.setAttribute("aria-label",tr("Torrent title"));heading.replaceWith(input);input.focus();input.select();
 var cancelled=false,done=false;
 function finish(){
  if(done)return;done=true;var value=cancelled?String(item.title||""):input.value;
  var next=el("h3",value||"Untitled torrent","torrent-title");next.tabIndex=0;next.setAttribute("role","button");next.title=tr("Click to edit");
  next.addEventListener("click",function(){editTorrentTitle(item,next);});
  next.addEventListener("keydown",function(e){if(e.key==="Enter"||e.key===" "){e.preventDefault();editTorrentTitle(item,next);}});
  input.replaceWith(next);if(!cancelled)saveLibraryField(item,"title",value,function(saved){next.textContent=saved||"Untitled torrent";});
 }
 input.addEventListener("keydown",function(e){if(e.key==="Enter"){e.preventDefault();input.blur();}else if(e.key==="Escape"){cancelled=true;input.blur();}});
 input.addEventListener("blur",finish,{once:true});
}
function renderLibrary(items){
 var target=$("library-rows");target.replaceChildren();
 items=Array.isArray(items)?items.filter(function(item){return item&&typeof item==="object"&&!Array.isArray(item);}):[];
 if(!items.length){target.append(el("p","Library is empty. Add a magnet or .torrent file above.","empty"));return;}
 items.forEach(function(item){
  var row=el("article",undefined,"row library-row"),copy=el("div",undefined,"row-copy");row.append(coverThumb(item));
  var title=el("h3",item.title||"Untitled torrent","torrent-title");title.tabIndex=0;title.setAttribute("role","button");title.title=tr("Click to edit");
  title.addEventListener("click",function(){editTorrentTitle(item,title);});
  title.addEventListener("keydown",function(e){if(e.key==="Enter"||e.key===" "){e.preventDefault();editTorrentTitle(item,title);}});
  copy.append(title,el("p",bytes(item.size)+" · "+tr("Files: ")+Number(item.file_count||0),"hint"));
  var category=el("input",undefined,"category-editor");category.type="text";category.maxLength=128;category.value=item.category||"";category.placeholder=tr("Category");category.setAttribute("aria-label",tr("Category"));
  category.addEventListener("keydown",function(e){if(e.key==="Enter"){e.preventDefault();category.blur();}});
  category.addEventListener("blur",function(){saveLibraryField(item,"category",category.value,function(value){category.value=value;});});
  copy.append(category);row.append(copy);
  var actions=el("div",undefined,"library-actions");
  actions.append(button("Remove",async function(){
   if(!await showDialog({kind:"confirm",title:"Remove torrent?",message:"Remove this torrent from the library?",acceptLabel:"Remove"}))return;
   await native("/torrents/"+encodeURIComponent(item.hash),undefined,"DELETE");status("Torrent removed.");await loadLibrary();
  },"danger"),button("Details / files",function(){return openDetails(item.hash);}));
  row.append(actions);target.append(row);
 });
}
async function loadLibrary(){
 
 var d=await native("/library/torrents?q="+encodeURIComponent($("library-query").value)+"&category="+encodeURIComponent($("library-category").value)+"&sort="+$("library-sort").value+"&order="+$("library-order").value+"&offset="+pageOffset+"&limit="+pageLimit);
 if(!d||typeof d!=="object")throw new Error("P2Play returned an invalid library response.");
 categoryOptions(d.categories);var items=Array.isArray(d.items)?d.items:[];renderLibrary(items);
 var filtered=Number.isFinite(Number(d.total))?Math.max(0,Number(d.total)):items.length;
 var overall=Number.isFinite(Number(d.total_all))?Math.max(filtered,Number(d.total_all)):filtered;
 $("library-filter-count").textContent="("+filtered+"/"+overall+")";
 var pages=Math.max(1,Math.ceil(filtered/pageLimit));
 $("library-page").textContent=tr("Page ")+(Math.floor(pageOffset/pageLimit)+1)+" / "+pages+" · "+filtered+tr(" items");
 $("library-pager").hidden=pages<=1;$("library-prev").disabled=pageOffset===0;$("library-next").disabled=pageOffset+pageLimit>=filtered;
 
}
function configureLibraryRefresh(){
 if(libraryRefreshTimer!==null){clearInterval(libraryRefreshTimer);libraryRefreshTimer=null;}
 var selector=$("library-refresh-interval");
 if(!selector||SILO)return;
 if(!SILO&&userKey){
  try{localStorage.setItem("p2play.libraryRefresh."+userKey,selector.value);}catch(_){}
 }
 var seconds=Number(selector.value);
 if(!Number.isFinite(seconds)||seconds<=0)return;
 libraryRefreshTimer=setInterval(function(){
  if(!session||activeMainView!=="library"||document.visibilityState==="hidden"||!$("torrent-detail").hidden)return;
  run(loadLibrary);
 },seconds*1000);
}
async function loadDetailCover(hash){var img=$("detail-cover-image"),note=$("detail-cover-source");img.hidden=true;img.removeAttribute("src");note.textContent=tr("No cover available.");try{var d=await native("/torrents/"+encodeURIComponent(hash));var t=d.torrent||{};if(t.poster&&t.poster.indexOf(appPath("/api/v1/torrents/"))===0){img.src=t.poster;img.hidden=false;note.textContent=tr("Custom poster");}else if(t.cover){img.src=coverUrl(hash);img.hidden=false;note.textContent=tr("Automatic poster");}else if(t.poster){note.textContent=tr("Poster URL: ")+t.poster;}else{note.textContent=tr("No cover available.");}}catch(e){note.textContent=e.message;}}
function refreshDetailCover(){return run(async function(){var d=await native("/torrents/"+encodeURIComponent(currentHash)+"/cover",{},"POST");status(d.status==="extracting"?"Looking for a cover…":"No cover requested.");var until=Date.now()+120000;while(Date.now()<until){await loadDetailCover(currentHash);if(!$("detail-cover-image").hidden)break;await new Promise(function(r){setTimeout(r,2000);});}await loadLibrary();});}
async function openDetails(hash){currentHash=hash;showView("library");$("torrent-detail").hidden=false;$("detail-heading").textContent=tr("Torrent ")+hash;$("detail-summary").replaceChildren();await loadDetailCover(hash);$("torrent-files").replaceChildren(el("p","Loading file list…","hint"));$("torrent-player").hidden=true;var d=await native("/torrents/"+encodeURIComponent(hash));var detail=d.details||{};["state_text","download_speed","upload_speed","total_wanted","total_wanted_done"].forEach(function(k){if(detail[k]!==undefined)$("detail-summary").append(el("p",k.replace(/_/g," ")+": "+detail[k]));});var actions=$("torrent-actions");actions.replaceChildren(button("Copy torrent stream link",async function(){var x=await native("/torrents/"+encodeURIComponent(hash)+"/share",{},"POST");var link=location.origin+x.url;if(navigator.clipboard&&navigator.clipboard.writeText)await navigator.clipboard.writeText(link);else{var input=document.createElement("textarea");input.value=link;document.body.append(input);input.select();document.execCommand("copy");input.remove();}status("Torrent stream link copied.");}),button("Mark all watched",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/viewed",{viewed:true},"PUT");await loadFiles();status("All files marked watched.");}),button("Mark all unviewed",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/viewed",{viewed:false},"PUT");await loadFiles();status("All files marked unviewed.");}),button("Reset all progress",async function(){if(!await showDialog({kind:"confirm",title:"Reset playback progress?",message:"Reset all watched marks and playback positions for this torrent?",acceptLabel:"Reset"}))return;await native("/torrents/"+encodeURIComponent(hash)+"/progress",undefined,"DELETE");await loadFiles();status("Progress reset.");},"danger"),button("Wake torrent",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/wake",{},"POST");status("Torrent activated.");}));
loadFiles();loadDiagnostics();}
function folderButtons(hash,path){var box=el("div",undefined,"file-actions");box.append(button("Mark folder watched",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/folders/viewed",{path:path,viewed:true},"PUT");await loadFiles();}),button("Reset folder",async function(){if(!await showDialog({kind:"confirm",title:"Reset folder progress?",message:"Reset playback progress for "+path+"?",acceptLabel:"Reset"}))return;await native("/torrents/"+encodeURIComponent(hash)+"/folders/progress?path="+encodeURIComponent(path),undefined,"DELETE");await loadFiles();},"danger"));return box;}
async function loadFiles(){if(!currentHash)return;var d,t=$("torrent-files");try{d=await native("/torrents/"+encodeURIComponent(currentHash)+"/files?wait=5");}catch(e){t.replaceChildren(el("p",tr("File list not available yet: ")+e.message,"hint"));status(e.message,true);return;}t.replaceChildren();var files=d.files||[],progress=d.file_progress||[],viewed=d.viewed_files||[];if(!files.length){t.append(el("p","No file metadata available yet. Wake the torrent and refresh.","empty"));return;}
var folders={};files.forEach(function(f){var parts=f.path.split("/");if(parts.length>1){var p="";for(var i=0;i<parts.length-1;i++){p=p?(p+"/"+parts[i]):parts[i];folders[p]=true;}}});
Object.keys(folders).sort().forEach(function(path){var row=el("article",undefined,"row file-row"),copy=el("div",undefined,"row-copy");copy.append(el("strong",path),el("p",tr("Folder · ")+tr("Files: ")+files.filter(function(f){return f.path.indexOf(path+"/")===0;}).length,"hint"));row.append(copy,folderButtons(currentHash,path));t.append(row);});
files.forEach(function(f){var p=progress.find(function(x){return x.id===f.id;})||{},isViewed=viewed.indexOf(f.id)>=0||p.viewed,row=el("article",undefined,"row file-row"),copy=el("div",undefined,"row-copy");copy.append(el("strong",f.path),el("p",bytes(f.length)+" · "+tr(isViewed?"Watched":"Not watched")+(p.duration>0?" · "+formatDuration(p.position||0)+" / "+formatDuration(p.duration):""),"hint"));var actions=el("div",undefined,"file-actions");actions.append(button("Play",function(){var v=$("torrent-player");v.src=appPath("/api/v1/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/stream");v.hidden=false;v.play().catch(function(){status("Press Play to start the stream.");});}),button("Copy stream link",async function(){var x=await native("/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/share",{},"POST");var link=location.origin+x.url;if(navigator.clipboard&&navigator.clipboard.writeText)await navigator.clipboard.writeText(link);else{var input=document.createElement("textarea");input.value=link;document.body.append(input);input.select();document.execCommand("copy");input.remove();}status("Stream link copied.");}),button(isViewed?"Mark unviewed":"Mark watched",async function(){await native("/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/viewed",{viewed:!isViewed},"PUT");await loadFiles();}),button("Reset progress",async function(){await native("/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/progress",undefined,"DELETE");await loadFiles();},"danger"));row.append(copy,actions);t.append(row);});}
async function loadDiagnostics(){var results=await Promise.allSettled([native("/torrents/"+encodeURIComponent(currentHash)+"/cache"),native("/torrents/"+encodeURIComponent(currentHash)+"/peers")]);$("torrent-cache").textContent=results[0].status==="fulfilled"?JSON.stringify(results[0].value,null,2):results[0].reason.message;var p=$("torrent-peers");p.replaceChildren();if(results[1].status!=="fulfilled"){p.append(el("p",results[1].reason.message,"hint"));return;}var peers=results[1].value.peers||[];if(!peers.length)p.append(el("p","No connected peers.","empty"));peers.forEach(function(peer){p.append(el("p",peer.address+" · "+(peer.client||"Unknown client")+" · "+Math.round(peer.progress*100)+"% · ↓ "+bytes(peer.download_speed)+"/s · ↑ "+bytes(peer.upload_speed)+"/s"));});}
async function loadContinue(){var d=await native("/progress"),t=$("continue-rows");t.replaceChildren();var list=d.progress||[];if(!list.length)t.append(el("p","Nothing to resume yet.","empty"));list.forEach(function(x){var row=el("article",undefined,"row"),copy=el("div",undefined,"row-copy");copy.append(el("h3",x.title||x.hash),el("p",x.file_name+" · "+formatDuration(x.position||0)+" / "+formatDuration(x.duration||0),"hint"));row.append(copy,button("Open",function(){showView("library");return openDetails(x.hash);}));t.append(row);});}
const settingFields={CacheSize:"setting-cache",DiskCacheSize:"setting-disk-cache-size",DiskCacheEnabled:"setting-disk-cache-enabled",CacheListsInMemory:"setting-cache-lists",CoverFiles:"setting-cover-files",CoverEmbedded:"setting-cover-embedded",CoverProbe:"setting-cover-probe",ReaderReadAHead:"setting-readahead",PreloadCache:"setting-preload",TorrentDisconnectTimeout:"setting-disconnect",ConnectionsLimit:"setting-connections",DownloadRateLimit:"setting-download",UploadRateLimit:"setting-upload",PeersListenPort:"setting-port",TorrentsSavePath:"setting-savepath",P2PDhtBootstrap:"setting-bootstrap",UseDisk:"setting-disk",DisableUpload:"setting-no-upload",DisableTCP:"setting-no-tcp",DisableUTP:"setting-no-utp",DisableDHT:"setting-no-dht",DisableMeshDirectory:"setting-no-mesh",DisableUPNP:"setting-no-upnp"};
async function loadServer(){var d=await native("/settings"),s=d.settings;Object.keys(settingFields).forEach(function(k){var n=$(settingFields[k]);if(n.type==="checkbox")n.checked=!!s[k];else if(k==="CacheSize")n.value=Math.round(s[k]/1048576);else if(k==="DiskCacheSize"){n.value=Math.round(s[k]/1073741824);$("setting-disk-cache-size-value").textContent=n.value+" GiB";}else n.value=s[k]===undefined?"":s[k];});
$("setting-readahead-value").textContent=$("setting-readahead").value+"%";
$("setting-preload-value").textContent=$("setting-preload").value+"%";
var stats=await native("/system/stats");$("server-stats").replaceChildren();[["Version",stats.version],["Uptime",formatDuration(stats.uptime_sec)],["Active sessions",stats.stats.active_sessions],["Active readers",stats.stats.active_readers],["RAM cache",bytes(stats.stats.cache_bytes==null?0:stats.stats.cache_bytes)+" / "+bytes(stats.stats.cache_capacity)],["Cached pieces",stats.stats.cache_pieces],["Disk cache",bytes(stats.stats.disk_cache_bytes)+" / "+bytes(stats.stats.disk_cache_capacity)]].forEach(function(x){var c=el("div",undefined,"stat");c.append(el("strong",String(x[1])),el("span",x[0]));$("server-stats").append(c);});}
$("server-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var v={};Object.keys(settingFields).forEach(function(k){var n=$(settingFields[k]);if(n.type==="checkbox")v[k]=n.checked;else if(n.type==="number"||n.type==="range")v[k]=Number(n.value);else v[k]=n.value;});v.CacheSize*=1048576;v.DiskCacheSize*=1073741824;var d=await native("/settings",{settings:v},"PUT");$("server-restart-note").hidden=!d.restart_required;status(d.restart_required?"Settings saved. Restart required for some changes.":"Server settings saved and applied.");});});
async function loadUsers(){
 var d=await native("/users"),t=$("users-rows");t.replaceChildren();
 (d.users||[]).forEach(function(u){
  var row=el("article",undefined,"row user-row"),copy=el("div",undefined,"row-copy");
  copy.append(el("h3",u.username),el("p",tr(u.rank>=100?"Owner":u.rank>=50?"Administrator":"User")+" · "+tr(u.is_banned?"Banned":"Active"),"hint"));
  var actions=el("div",undefined,"user-actions"),canManage=u.id!=="owner"&&session.rank>u.rank;
  if(canManage){
   var role=document.createElement("select");[[10,"User"],[50,"Administrator"]].forEach(function(x){var o=el("option",x[1]);o.value=x[0];role.append(o);});role.value=u.rank;
   actions.append(role,button("Set role",async function(){await native("/users/"+encodeURIComponent(u.id)+"/rank",{rank:Number(role.value)},"POST");await loadUsers();}),button(u.is_banned?"Unban":"Ban",async function(){await native("/users/"+encodeURIComponent(u.id),{banned:!u.is_banned},"PUT");await loadUsers();}),button("Reset password",async function(){var p=await showDialog({kind:"prompt",title:"Reset password",message:tr("Set a new password for ")+u.username+".",inputLabel:"New password",inputType:"password",required:true,acceptLabel:"Save"});if(p===null)return;await native("/users/"+encodeURIComponent(u.id),{password:p},"PUT");status("Password changed.");}),button("Delete",async function(){if(!await showDialog({kind:"confirm",title:"Delete user?",message:tr("Delete user ")+u.username+"?",acceptLabel:"Delete"}))return;await native("/users/"+encodeURIComponent(u.id),undefined,"DELETE");await loadUsers();},"danger"));
  }
  if(canManage||session.user===u.username){
   actions.append(button("Reset API token",async function(){if(!await showDialog({kind:"confirm",title:"Reset API token?",message:"Existing stream links will stop working. Continue?",acceptLabel:"Reset"}))return;var x=await native("/users/"+encodeURIComponent(u.id)+"/regenerate-token",{},"POST");await showDialog({kind:"notice",title:"New API token",message:"Copy this token now. It will only be shown once.",secret:x.token,acceptLabel:"Close"});status("API token reset.");}));
  }
  row.append(copy,actions);t.append(row);
 });
}$("user-add-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var d=await native("/users",{username:$("new-user-name").value.trim(),password:$("new-user-password").value,rank:Number($("new-user-rank").value)},"POST");$("user-add-form").reset();status(tr("Created user ")+d.username+".");await loadUsers();});});
$("library-add-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var link=$("library-link").value.trim(),file=$("library-file").files[0],posterUrl=$("library-poster-url").value.trim(),posterFile=$("library-poster-file").files[0];if(!!link===!!file)throw new Error("Choose exactly one magnet/link or one .torrent file.");if(file&&!file.size)throw new Error("The selected file is empty.");if(posterUrl&&posterFile)throw new Error("Choose either a poster URL or an image file.");if(posterFile&&!posterFile.size)throw new Error("The selected poster file is empty.");var form=new FormData();if(file)form.append("file",file,file.name);else form.append("link",link);form.append("title",$("library-title").value.trim());form.append("category",$("library-new-category").value.trim());if(posterFile)form.append("poster_file",posterFile,posterFile.name);else if(posterUrl)form.append("poster_url",posterUrl);await native("/torrents",form,"POST");$("library-add-form").reset();closeModal("add-modal");await loadLibrary();status("Torrent added.");});});
$("library-import-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var f=$("library-import-file").files[0];if(!f)throw new Error("Choose a .torrs file.");var form=new FormData();form.append("file",f,f.name);var d=await native("/library/import",form,"POST");$("library-import-form").reset();await loadLibrary();status(tr("Import complete: ")+d.added+tr(" added, ")+d.skipped+tr(" already present, ")+d.rejected+tr(" rejected."));});});
$("library-filter-form").addEventListener("submit",function(e){e.preventDefault();pageOffset=0;run(loadLibrary);});$("library-refresh-interval").addEventListener("change",configureLibraryRefresh);$("library-prev").addEventListener("click",function(){pageOffset=Math.max(0,pageOffset-pageLimit);run(loadLibrary);});$("library-next").addEventListener("click",function(){pageOffset+=pageLimit;run(loadLibrary);});
$("detail-cover-refresh").addEventListener("click",function(){refreshDetailCover();});
$("detail-close").addEventListener("click",function(){$("torrent-detail").hidden=true;$("torrent-player").pause();$("torrent-player").removeAttribute("src");});$("files-refresh").addEventListener("click",function(){loadFiles();});$("cache-refresh").addEventListener("click",function(){loadDiagnostics();});$("peers-refresh").addEventListener("click",function(){loadDiagnostics();});
["continue-refresh","server-refresh","users-refresh"].forEach(function(id){$(id).addEventListener("click",function(){run(id==="continue-refresh"?loadContinue:id==="server-refresh"?loadServer:loadUsers);});});
$("search-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var d=await host.search($("query").value,$("tag").value);renderResults(d.results||[]);status((d.results||[]).length+" "+tr(" releases found."));});});
$("reload-lists").addEventListener("click",function(){run(loadLists);});$("subscribe-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){await host.subscribe($("pubkey").value.trim());status("Subscription requested.");});});
$("settings-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var token=$("daemon-token").value;$("daemon-token").value="";await host.settings({url:$("daemon-url").value.trim(),token:token,clear_token:$("clear-token").checked});await loadConnection();status("Connection saved.");});});
$("login-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){await request(appPath("/api/v1/auth/login"),{username:$("username").value.trim(),password:$("password").value});authEpoch++;$("password").value="";await start();});});
$("logout").addEventListener("click",function(){run(async function(){await request(appPath("/api/v1/auth/logout"),{});showLogin();});});
var dialogResolve=null;
function closeDialog(result){
 if(!dialogResolve)return;
 var resolve=dialogResolve;dialogResolve=null;closeModal("dialog-modal");resolve(result);
}
function showDialog(options){
 options=options||{};
 return new Promise(function(resolve){
  dialogResolve=resolve;
  var kind=options.kind||"notice",inputWrap=$("dialog-input-wrap"),secretWrap=$("dialog-secret-wrap");
  $("dialog-title").textContent=tr(options.title||"Notice");
  $("dialog-message").textContent=tr(options.message||"");$("dialog-message").hidden=!options.message;
  inputWrap.hidden=kind!=="prompt";secretWrap.hidden=!options.secret;
  $("dialog-input-label").textContent=tr(options.inputLabel||"Value");
  $("dialog-input").value=options.initialValue||"";$("dialog-input").type=options.inputType||"text";$("dialog-input").required=!!options.required;
  $("dialog-secret").value=options.secret||"";$("dialog-secret-label").textContent=tr(options.secretLabel||"Token");
  $("dialog-cancel").hidden=kind==="notice";$("dialog-accept").textContent=tr(options.acceptLabel||(kind==="confirm"?"Confirm":kind==="prompt"?"Save":"OK"));
  $("dialog-cancel").textContent=tr(options.cancelLabel||"Cancel");
  openModal("dialog-modal");
  if(kind==="prompt"){setTimeout(function(){$("dialog-input").focus();$("dialog-input").select();},0);}else if(options.secret){setTimeout(function(){$("dialog-secret").focus();$("dialog-secret").select();},0);}else $("dialog-accept").focus();
 });
}
function syncModalState(){
 var opened=!$("add-modal").hidden||!$("settings-modal").hidden||!$("dialog-modal").hidden;
 document.body.classList.toggle("modal-open",opened);
}
function openModal(id){$(id).hidden=false;syncModalState();}
function closeModal(id){$(id).hidden=true;syncModalState();}
function closeAccountMenu(){$("account-popover").hidden=true;$("account-menu-trigger").setAttribute("aria-expanded","false");}
function openAccountMenu(){$("account-popover").hidden=false;$("account-menu-trigger").setAttribute("aria-expanded","true");}
function applyTheme(){
 document.documentElement.dataset.theme=theme;
 var icons={
  light:'<circle cx="12" cy="12" r="4"></circle><path d="M12 2v2m0 16v2M4.93 4.93l1.42 1.42m11.3 11.3 1.42 1.42M2 12h2m16 0h2M4.93 19.07l1.42-1.42m11.3-11.3 1.42-1.42"></path>',
  dark:'<path d="M20.5 14.2A8.5 8.5 0 0 1 9.8 3.5 8.5 8.5 0 1 0 20.5 14.2Z"></path>',
  auto:'<rect x="3" y="4" width="18" height="13" rx="2"></rect><path d="M8 21h8m-4-4v4"></path>'
 };
 var label=tr("Theme")+": "+tr(theme==="auto"?"Auto":theme==="dark"?"Dark":"Light");
 $("theme-icon").innerHTML=icons[theme]||icons.light;
 $("theme-toggle").setAttribute("aria-label",label);$("theme-toggle").title=label;
}
function localizeStatic(){
 document.documentElement.lang=language;
 document.querySelectorAll("body *").forEach(function(node){
  Array.from(node.childNodes).forEach(function(child){
   if(child.nodeType!==3||!child.nodeValue.trim())return;
   if(child.__i18nSource===undefined)child.__i18nSource=child.nodeValue.trim();
   var source=typeof child.nodeValue==="string"?child.nodeValue:"";
   var value=tr(child.__i18nSource),leading=(source.match(/^\s*/)||[""])[0],trailing=(source.match(/\s*$/)||[""])[0];
   child.nodeValue=leading+value+trailing;
  });
  ["placeholder","title","aria-label"].forEach(function(attr){
   if(!node.hasAttribute||!node.hasAttribute(attr))return;
   var key="__i18n_"+attr;
   if(node[key]===undefined)node[key]=node.getAttribute(attr);
   node.setAttribute(attr,tr(node[key]));
  });
 });
}
async function loadLocale(){
 try{
  var response=await fetch((SILO?base:appPath("/ui"))+"/lang/"+language+".json",{credentials:"same-origin",cache:"no-store"});
  if(!response.ok)throw new Error("translation file unavailable");
  locale=await response.json();
 }catch(_){locale={};}
 localizeStatic();
 applyTheme();
 $("language-select").value=language;
}
async function loadPreferences(){
 var values={language:"en",theme:"light"};
 if(SILO){
  try{values=Object.assign(values,JSON.parse(localStorage.getItem("p2play.ui."+userKey)||"{}"));}catch(_){}
 }else{
  try{var data=await native("/preferences");values=Object.assign(values,data.preferences||{});}catch(_){}
 }
 language=values.language==="ru"?"ru":"en";
 theme=values.theme==="dark"||values.theme==="auto"?values.theme:"light";
 applyTheme();
}
async function savePreference(name,value){
 if(SILO){
  var current={};try{current=JSON.parse(localStorage.getItem("p2play.ui."+userKey)||"{}");}catch(_){}
  current[name]=value;localStorage.setItem("p2play.ui."+userKey,JSON.stringify(current));
 }else{
  var payload={};payload[name]=value;
  await native("/preferences",payload,"PUT");
 }
}
async function setLanguage(next){
 language=next==="ru"?"ru":"en";
 await loadLocale();
 await savePreference("language",language);
 Object.keys(renderedResultSets).forEach(function(id){
  var saved=renderedResultSets[id];
  if($(id))renderResults(saved.results,id,saved.onPick);
 });
 if(!SILO){await loadLibrary();await loadContinue();}
 if(!$("settings-modal").hidden){
  var selected=document.querySelector("[data-settings-view].active");
  if(selected){
   var view=selected.dataset.settingsView;
   if(view==="lists")await loadLists();
   else if(view==="server")await loadServer();
   else if(view==="users")await loadUsers();
   else if(view==="settings")await loadConnection();
  }
 }
 if(currentHash&&$("torrent-detail")&&!$("torrent-detail").hidden&&typeof loadDetailCover==="function")await loadDetailCover(currentHash);
 status("Language saved.");
}
async function setTheme(next){
 theme=next==="dark"||next==="auto"?next:"light";
 applyTheme();await savePreference("theme",theme);status("Theme saved.");
}
async function toggleTheme(){
 var modes=["light","dark","auto"],index=modes.indexOf(theme);
 await setTheme(modes[(index+1)%modes.length]);
}
function selectCatalogResult(record){
 var link=record.torrs||(record.hash.length===64?"magnet:?xt=urn:btmh:1220"+record.hash:"magnet:?xt=urn:btih:"+record.hash);
 $("library-link").value=link;
 $("library-title").value=record.title||"";
 var tags=Array.isArray(record.tags)?record.tags.filter(function(x){return typeof x==="string";}):[];
 $("library-new-category").value=tags.length?tags[0]:"";
 $("add-search-panel").hidden=true;
 $("add-form-panel").hidden=false;
 $("library-link").focus();
 status("Release selected. Review details and add it.");
}
async function start(){
 var current=await host.session();
 var initialLoadError="";
 session=current;
 userKey=String(session.user||"user");
 document.querySelectorAll(".silo-only").forEach(function(n){n.hidden=!SILO;});
 document.querySelectorAll(".core-only").forEach(function(n){n.hidden=SILO;});
 $("account-menu-wrap").hidden=false;
 $("account-menu-trigger").classList.toggle("account-trigger-static",SILO);
 $("account-menu-trigger").setAttribute("aria-haspopup",String(!SILO));
 $("identity-name").textContent=session.user||"User";
 $("identity-role").textContent=tr(session.rank>=100?"Owner":session.rank>=50?"Administrator":"User");
 $("logout").hidden=SILO;closeAccountMenu();
 $("add-open").hidden=SILO;
 $("settings-open").hidden=false;
 $("users-tab").hidden=SILO||session.rank<50;
 $("server-tab").hidden=SILO||session.rank<100;
 $("subscribe-form").hidden=!session.can_admin;
 // A verified session switches the shell immediately; secondary data loads
 // must not leave the login form visible over the authenticated interface.
 showView(SILO?"search":"library");
 $("login-view").hidden=true;
 await loadPreferences();
 await loadLocale();
 if(!SILO){
  try{
   var savedRefresh=localStorage.getItem("p2play.libraryRefresh."+userKey);
   if(["5","10","30","60","0"].indexOf(savedRefresh)>=0)$("library-refresh-interval").value=savedRefresh;
  }catch(_){}
  configureLibraryRefresh();
  try{await loadLibrary();}catch(e){initialLoadError=e.message||"Could not load the library.";}
  try{await loadContinue();}catch(e){initialLoadError=e.message||"Could not load playback progress.";}
 }
 $("login-view").hidden=true;
 if(initialLoadError)status(initialLoadError,true);
 else status(SILO?"Search catalogs.":"Library is ready.");
}
$("add-open").addEventListener("click",function(){$("add-form-panel").hidden=false;$("add-search-panel").hidden=true;$("library-add-form").reset();openModal("add-modal");$("library-link").focus();});
$("add-close").addEventListener("click",function(){closeModal("add-modal");});
$("add-cancel").addEventListener("click",function(){closeModal("add-modal");});
$("add-search-open").addEventListener("click",function(){$("add-form-panel").hidden=true;$("add-search-panel").hidden=false;$("add-query").focus();});
$("add-search-close").addEventListener("click",function(){$("add-search-panel").hidden=true;$("add-form-panel").hidden=false;});
$("add-search-cancel").addEventListener("click",function(){$("add-search-panel").hidden=true;$("add-form-panel").hidden=false;});
$("add-search-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var data=await host.search($("add-query").value,$("add-tag").value);renderResults(data.results||[],"add-search-results",selectCatalogResult);});});
$("settings-open").addEventListener("click",function(){showView("preferences");});
$("settings-close").addEventListener("click",function(){showView(activeMainView);});
document.querySelectorAll("[data-settings-view]").forEach(function(tab){tab.addEventListener("click",function(){run(async function(){var view=tab.dataset.settingsView;showView(view);if(view==="lists")await loadLists();else if(view==="server")await loadServer();else if(view==="users")await loadUsers();else if(view==="settings")await loadConnection();});});});
$("theme-toggle").addEventListener("click",function(){run(toggleTheme);});
$("setting-disk-cache-enabled").addEventListener("change",function(){$("disk-cache-size-control").hidden=!this.checked;});
$("account-menu-trigger").addEventListener("click",function(){if(!SILO)openAccountMenu();});
$("account-menu-wrap").addEventListener("mouseenter",function(){if(!SILO)openAccountMenu();});
$("account-menu-wrap").addEventListener("mouseleave",function(){if(!SILO)closeAccountMenu();});
document.addEventListener("pointerdown",function(e){if(!$("account-menu-wrap").contains(e.target))closeAccountMenu();});
$("language-select").addEventListener("change",function(){run(function(){return setLanguage($("language-select").value);});});
document.querySelectorAll(".modal-backdrop").forEach(function(modal){modal.addEventListener("click",function(e){if(e.target!==modal)return;if(modal.id==="settings-modal")showView(activeMainView);else if(modal.id==="dialog-modal")closeDialog(null);else closeModal(modal.id);});});
$("dialog-form").addEventListener("submit",function(e){e.preventDefault();var promptMode=!$("dialog-input-wrap").hidden,value=$("dialog-input").value;if(promptMode&&$("dialog-input").required&&!value.trim()){$("dialog-message").textContent=tr("A value is required.");return;}closeDialog(promptMode?value:true);});
$("dialog-cancel").addEventListener("click",function(){closeDialog(null);});
$("dialog-close").addEventListener("click",function(){closeDialog(null);});
document.addEventListener("keydown",function(e){if(e.key!=="Escape")return;if(!$("dialog-modal").hidden)closeDialog(null);else if(!$("add-modal").hidden)closeModal("add-modal");else if(!$("settings-modal").hidden)showView(activeMainView);else closeAccountMenu();});
$("setting-disk-cache-size").addEventListener("input",function(){$("setting-disk-cache-size-value").textContent=this.value+" GiB";});
$("setting-readahead").addEventListener("input",function(){$("setting-readahead-value").textContent=this.value+"%";});
$("setting-preload").addEventListener("input",function(){$("setting-preload-value").textContent=this.value+"%";});
document.title="MediaMesh · "+HOST;run(start);
}());
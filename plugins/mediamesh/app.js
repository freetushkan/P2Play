(function () {
"use strict";
var SILO=location.pathname.indexOf("/plugins/")===0, HOST=SILO?"Silo":"P2Play", base="/plugins/mediamesh",session,busy=false,currentHash="",pageOffset=0,pageLimit=30;
var locale={},language="en",theme="light",userKey="";
var $=function(id){return document.getElementById(id);};
function tr(value){return typeof value==="string"&&locale[value]?locale[value]:value;}
function status(message,bad){$("status").textContent=tr(message);$("status").className=bad?"error":"";}
async function request(url,body,headers,method){
 var opts={method:method||(body===undefined?"GET":"POST"),credentials:"same-origin",headers:{Accept:"application/json"},cache:"no-store"};
 if(body!==undefined){if(body instanceof FormData)opts.body=body;else{opts.headers["Content-Type"]="application/json";opts.body=JSON.stringify(body);}}
 Object.keys(headers||{}).forEach(function(k){opts.headers[k]=headers[k];});
 var res=await fetch(url,opts),data;
 if(res.status===204)return {};
 try{data=await res.json();}catch(_){throw new Error(HOST+tr(" returned an unreadable response."));}
 if(res.status===401&&!SILO){showLogin();throw new Error("Log in to this P2Play node.");}
 if(!res.ok)throw new Error(data.error||tr("Request failed (")+res.status+").");
 return data;
}
function api(path,body,method){return request(base+path,body,session?{"X-MediaMesh-CSRF":session.csrf}:{},method);}
function core(path,body,method){return request("/api"+path,body,{},method);}
function native(path,body,method){return request("/api/v1"+path,body,{},method);}
var host=SILO?{
 session:function(){return api("/api/session");},
 search:function(q,tag){return api("/api/search?q="+encodeURIComponent(q)+"&tag="+encodeURIComponent(tag));},
 lists:function(){return api("/api/lists");},discover:function(){return api("/api/discover");},
 subscribe:function(pubkey){return api("/api/subscribe",{pubkey:pubkey});},
 add:function(r){return api("/api/add",{hash:r.hash});},
 settings:function(body){return api("/api/settings",body);}
}:{
 session:async function(){var me=await core("/auth/me");return{rank:me.rank,csrf:"",can_admin:me.rank>=50,user:me.username};},
 search:async function(q,tag){var d=await core("/catalog/search?q="+encodeURIComponent(q)+"&tag="+encodeURIComponent(tag)+"&limit=50");return{results:(d.results||[]).map(function(r){return Object.assign({},r,{supported:true,tags:r.tags||[]});})};},
 lists:function(){return core("/lists");},discover:function(){return core("/lists/discover");},
 subscribe:function(pubkey){return core("/lists/"+encodeURIComponent(pubkey.toLowerCase()),{});},
 add:function(r){var link=r.torrs||(r.hash.length===64?"magnet:?xt=urn:btmh:1220"+r.hash:"magnet:?xt=urn:btih:"+r.hash);return native("/torrents",{link:link,title:r.title},"POST");}
};
function showLogin(){$("login-view").hidden=false;["search","lists","library","continue","server","users","settings","preferences"].forEach(function(x){var n=$(x+"-view");if(n)n.hidden=true;});$("settings-modal").hidden=true;$("add-modal").hidden=true;syncModalState();}
async function run(fn){if(busy)return;busy=true;document.body.setAttribute("aria-busy","true");try{await fn();}catch(e){status(e.message||"Request failed.",true);}finally{busy=false;document.body.removeAttribute("aria-busy");}}
function el(tag,value,cls){var n=document.createElement(tag);if(value!==undefined)n.textContent=tr(value);if(cls)n.className=cls;return n;}
function button(label,fn,cls){var b=el("button",label,cls);b.type="button";b.addEventListener("click",function(){run(function(){return fn(b);});});return b;}
function bytes(n){n=Number(n);if(!Number.isFinite(n)||n<0)return tr("Size unknown");if(n===0)return tr("0 B");var u=["B","KiB","MiB","GiB","TiB"],i=0;while(n>=1024&&i<u.length-1){n/=1024;i++;}return n.toFixed(i?1:0)+" "+u[i];}
function renderResults(results,targetId,onPick){var t=$(targetId||"results");t.replaceChildren();if(!results.length)t.append(el("p","No releases found.","empty"));results.forEach(function(r){var row=el("article",undefined,"row"),copy=el("div",undefined,"row-copy");copy.append(el("h3",r.title||r.hash),el("p",bytes(r.size)+" · "+(r.files||0)+" "+tr("files")),el("p",(r.tags||[]).join(", "),"hint"),el("code",r.hash));var action=onPick?button("Select",function(){onPick(r);}):button(tr("Add to ")+HOST,async function(b){b.disabled=true;await host.add(r);b.textContent=tr("Added");status(tr("Added to ")+HOST+".");});row.append(copy,action);t.append(row);});}
function renderLists(target,lists,discovery){target.replaceChildren();if(!lists.length)target.append(el("p",discovery?"No lists advertised yet.":"No subscribed lists.","empty"));(lists||[]).forEach(function(l){var row=el("article",undefined,"row"),copy=el("div",undefined,"row-copy");copy.append(el("h3",l.name||"Unnamed list"),el("p",l.description||"No description"),el("p",tr("Version ")+l.seq,"hint"),el("code",l.pubkey));row.append(copy);if(session.can_admin)row.append(button(discovery?"Subscribe":"Request refresh",async function(){await host.subscribe(l.pubkey);status("Subscription requested.");}));target.append(row);});}
async function loadLists(){var a=await host.lists();renderLists($("lists"),a.lists,false);var b=await host.discover();renderLists($("discover"),b.lists,true);status("Lists loaded.");}
var activeMainView="library";
function showView(view){
 $("login-view").hidden=true;
 var main=view==="search"||view==="library";
 if(main)activeMainView=view;
 ["search","library"].forEach(function(x){var n=$(x+"-view");if(n)n.hidden=x!==view;});
 var pane=view==="preferences"?"preferences-view":view+"-view";
 ["preferences-view","lists-view","continue-view","server-view","users-view","settings-view"].forEach(function(id){var n=$(id);if(n)n.hidden=main||id!==pane;});
 $("settings-modal").hidden=main;
 if(!main){document.querySelectorAll("[data-settings-view]").forEach(function(b){b.classList.toggle("active",b.dataset.settingsView===view);});}
 document.querySelectorAll("[data-view]").forEach(function(b){b.setAttribute("aria-pressed",String(b.dataset.view===view));});
 syncModalState();
}
document.querySelectorAll("[data-view]").forEach(function(b){b.addEventListener("click",function(){run(async function(){if(!session)return;var v=b.dataset.view;showView(v);if(v==="lists")await loadLists();else if(v==="library")await loadLibrary();else if(v==="continue")await loadContinue();else if(v==="server")await loadServer();else if(v==="users")await loadUsers();else if(v==="settings")await loadConnection();else status("Search subscribed catalogs.");});});});
async function loadConnection(){var cfg=await host.settings();$("daemon-url").value=cfg.url;$("daemon-token").value="";$("clear-token").checked=false;$("token-state").textContent=tr(cfg.token_configured?"A token is stored on the Silo server.":"No daemon token is stored.");}
function categoryOptions(values){var s=$("library-category"),old=s.value;s.replaceChildren();[["","All categories"],["none","Uncategorized"]].concat((values||[]).map(function(x){return[x,x];})).forEach(function(x){var o=el("option",x[1]);o.value=x[0];s.append(o);});if(Array.from(s.options).some(function(o){return o.value===old;}))s.value=old;}
function coverUrl(hash){return "/api/v1/torrents/"+encodeURIComponent(hash)+"/cover";}
function coverThumb(item){var src=item.poster&&item.poster.indexOf("/api/v1/torrents/")===0?item.poster:(item.cover?coverUrl(item.hash):"");if(!src)return el("div",undefined,"cover-placeholder");var img=el("img",undefined,"cover-thumb");img.src=src;img.alt="";img.loading="lazy";return img;}
function renderLibrary(items){var t=$("library-rows");t.replaceChildren();if(!items.length){t.append(el("p","Your library is empty. Use Add torrent to add something.","empty"));return;}items.forEach(function(item){var row=el("article",undefined,"row library-row"),copy=el("div",undefined,"row-copy");row.append(coverThumb(item));copy.append(el("h3",item.title||item.hash),el("p",bytes(item.size)+" · "+(item.file_count||0)+" "+tr("files"),"hint"),el("p",item.state_text||tr("Torrent")+(item.cover?" · "+tr("cover: ")+item.cover_source:""),"hint"),el("code",item.hash));
var title=el("label","Display title"),ti=el("input");ti.value=item.title||"";ti.maxLength=512;ti.setAttribute("aria-label","Title for "+item.hash);title.append(ti);
var cat=el("label","Category"),ci=el("input");ci.value=item.category||"";ci.maxLength=128;ci.setAttribute("aria-label","Category for "+item.hash);cat.append(ci);copy.append(title,cat);
var actions=el("div",undefined,"library-actions");actions.append(button("Save changes",async function(){await native("/torrents/"+encodeURIComponent(item.hash),{title:ti.value.trim(),category:ci.value.trim()},"PUT");status("Title and category saved.");await loadLibrary();}),button("Details / files",function(){return openDetails(item.hash);}),button("Remove",async function(){if(!confirm(tr("Remove this torrent from your library? Other users are not affected.")))return;await native("/torrents/"+encodeURIComponent(item.hash),undefined,"DELETE");status("Removed from your library.");await loadLibrary();},"danger"));row.append(copy,actions);t.append(row);});}
async function loadLibrary(){status("Loading library…");var d=await native("/library/torrents?q="+encodeURIComponent($("library-query").value)+"&category="+encodeURIComponent($("library-category").value)+"&sort="+$("library-sort").value+"&order="+$("library-order").value+"&offset="+pageOffset+"&limit="+pageLimit);categoryOptions(d.categories);renderLibrary(d.items||[]);$("library-page").textContent=tr("Page ")+(Math.floor(pageOffset/pageLimit)+1)+" · "+d.total+tr(" items");$("library-prev").disabled=pageOffset===0;$("library-next").disabled=pageOffset+pageLimit>=d.total;status(d.total+" torrent(s) in your library.");}
async function loadDetailCover(hash){var img=$("detail-cover-image"),note=$("detail-cover-source");img.hidden=true;img.removeAttribute("src");note.textContent="No cover yet.";try{var d=await native("/torrents/"+encodeURIComponent(hash));var t=d.torrent||{};if(t.poster&&t.poster.indexOf("/api/v1/torrents/")===0){img.src=t.poster;img.hidden=false;note.textContent="Custom poster · stored on this P2Play server";}else if(t.cover){img.src=coverUrl(hash);img.hidden=false;note.textContent=tr("Cover: ")+t.cover_source;}else if(t.poster){note.textContent=tr("External poster metadata: ")+t.poster;}else{note.textContent=tr("No cover yet. The server looks for an image inside the torrent, then probes a video frame with ffmpeg.");}}catch(e){note.textContent=e.message;}}
function refreshDetailCover(){return run(async function(){var d=await native("/torrents/"+encodeURIComponent(currentHash)+"/cover",{},"POST");status(d.status==="extracting"?"Looking for a cover…":"No cover requested.");var until=Date.now()+120000;while(Date.now()<until){await loadDetailCover(currentHash);if(!$("detail-cover-image").hidden)break;await new Promise(function(r){setTimeout(r,2000);});}await loadLibrary();});}
async function openDetails(hash){currentHash=hash;showView("library");$("torrent-detail").hidden=false;$("detail-heading").textContent=tr("Torrent ")+hash;$("detail-summary").replaceChildren();await loadDetailCover(hash);$("torrent-files").replaceChildren(el("p","Loading file list…","hint"));$("torrent-player").hidden=true;var d=await native("/torrents/"+encodeURIComponent(hash));var detail=d.details||{};["state_text","download_speed","upload_speed","total_wanted","total_wanted_done"].forEach(function(k){if(detail[k]!==undefined)$("detail-summary").append(el("p",k.replace(/_/g," ")+": "+detail[k]));});var actions=$("torrent-actions");actions.replaceChildren(button("Mark all watched",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/viewed",{viewed:true},"PUT");await loadFiles();status("All files marked watched.");}),button("Mark all unviewed",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/viewed",{viewed:false},"PUT");await loadFiles();status("All files marked unviewed.");}),button("Reset all progress",async function(){if(!confirm(tr("Reset all watched marks and playback positions for this torrent?")))return;await native("/torrents/"+encodeURIComponent(hash)+"/progress",undefined,"DELETE");await loadFiles();status("Progress reset.");},"danger"),button("Wake torrent",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/wake",{},"POST");status("Torrent activated.");}));
loadFiles();loadDiagnostics();}
function folderButtons(hash,path){var box=el("div",undefined,"file-actions");box.append(button("Mark folder watched",async function(){await native("/torrents/"+encodeURIComponent(hash)+"/folders/viewed",{path:path,viewed:true},"PUT");await loadFiles();}),button("Reset folder",async function(){if(!confirm("Reset playback progress for "+path+"?"))return;await native("/torrents/"+encodeURIComponent(hash)+"/folders/progress?path="+encodeURIComponent(path),undefined,"DELETE");await loadFiles();},"danger"));return box;}
async function loadFiles(){if(!currentHash)return;var d,t=$("torrent-files");try{d=await native("/torrents/"+encodeURIComponent(currentHash)+"/files?wait=5");}catch(e){t.replaceChildren(el("p",tr("File list not available yet: ")+e.message,"hint"));status(e.message,true);return;}t.replaceChildren();var files=d.files||[],progress=d.file_progress||[],viewed=d.viewed_files||[];if(!files.length){t.append(el("p","No file metadata available yet. Wake the torrent and refresh.","empty"));return;}
var folders={};files.forEach(function(f){var parts=f.path.split("/");if(parts.length>1){var p="";for(var i=0;i<parts.length-1;i++){p=p?(p+"/"+parts[i]):parts[i];folders[p]=true;}}});
Object.keys(folders).sort().forEach(function(path){var row=el("article",undefined,"row file-row"),copy=el("div",undefined,"row-copy");copy.append(el("strong",path),el("p",tr("Folder · ")+files.filter(function(f){return f.path.indexOf(path+"/")===0;}).length+" "+tr("files"),"hint"));row.append(copy,folderButtons(currentHash,path));t.append(row);});
files.forEach(function(f){var p=progress.find(function(x){return x.id===f.id;})||{},isViewed=viewed.indexOf(f.id)>=0||p.viewed,row=el("article",undefined,"row file-row"),copy=el("div",undefined,"row-copy");copy.append(el("strong",f.path),el("p",bytes(f.length)+" · "+tr(isViewed?"Watched":"Not watched"))+(p.duration>0?" · "+Math.floor(p.position||0)+"/"+Math.floor(p.duration)+" s":""),"hint"));var actions=el("div",undefined,"file-actions");actions.append(button("Play",function(){var v=$("torrent-player");v.src="/api/v1/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/stream";v.hidden=false;v.play().catch(function(){status("Press Play to start the stream.");});}),button(isViewed?"Mark unviewed":"Mark watched",async function(){await native("/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/viewed",{viewed:!isViewed},"PUT");await loadFiles();}),button("Reset progress",async function(){await native("/torrents/"+encodeURIComponent(currentHash)+"/files/"+f.id+"/progress",undefined,"DELETE");await loadFiles();},"danger"));row.append(copy,actions);t.append(row);});}
async function loadDiagnostics(){var results=await Promise.allSettled([native("/torrents/"+encodeURIComponent(currentHash)+"/cache"),native("/torrents/"+encodeURIComponent(currentHash)+"/peers")]);$("torrent-cache").textContent=results[0].status==="fulfilled"?JSON.stringify(results[0].value,null,2):results[0].reason.message;var p=$("torrent-peers");p.replaceChildren();if(results[1].status!=="fulfilled"){p.append(el("p",results[1].reason.message,"hint"));return;}var peers=results[1].value.peers||[];if(!peers.length)p.append(el("p","No connected peers.","empty"));peers.forEach(function(peer){p.append(el("p",peer.address+" · "+(peer.client||"Unknown client")+" · "+Math.round(peer.progress*100)+"% · ↓ "+bytes(peer.download_speed)+"/s · ↑ "+bytes(peer.upload_speed)+"/s"));});}
async function loadContinue(){var d=await native("/progress"),t=$("continue-rows");t.replaceChildren();var list=d.progress||[];if(!list.length)t.append(el("p","Nothing to resume yet.","empty"));list.forEach(function(x){var row=el("article",undefined,"row"),copy=el("div",undefined,"row-copy");copy.append(el("h3",x.title||x.hash),el("p",x.file_name+" · "+Math.floor(x.position||0)+" / "+Math.floor(x.duration||0)+" s","hint"));row.append(copy,button("Open",function(){showView("library");return openDetails(x.hash);}));t.append(row);});status(list.length+" "+tr(" playback records."));}
const settingFields={CacheSize:"setting-cache",DiskCacheSize:"setting-disk-cache-size",DiskCacheEnabled:"setting-disk-cache-enabled",CacheListsInMemory:"setting-cache-lists",ReaderReadAHead:"setting-readahead",PreloadCache:"setting-preload",TorrentDisconnectTimeout:"setting-disconnect",ConnectionsLimit:"setting-connections",DownloadRateLimit:"setting-download",UploadRateLimit:"setting-upload",PeersListenPort:"setting-port",TorrentsSavePath:"setting-savepath",P2PDhtBootstrap:"setting-bootstrap",UseDisk:"setting-disk",DisableUpload:"setting-no-upload",DisableTCP:"setting-no-tcp",DisableUTP:"setting-no-utp",DisableDHT:"setting-no-dht",DisableMeshDirectory:"setting-no-mesh",DisableUPNP:"setting-no-upnp"};
async function loadServer(){var d=await native("/settings"),s=d.settings;Object.keys(settingFields).forEach(function(k){var n=$(settingFields[k]);if(n.type==="checkbox")n.checked=!!s[k];else if(k==="CacheSize")n.value=Math.round(s[k]/1048576);else if(k==="DiskCacheSize"){n.value=Math.round(s[k]/1073741824);$("setting-disk-cache-size-value").textContent=n.value+" GiB";}else n.value=s[k]===undefined?"":s[k];});
$("setting-readahead-value").textContent=$("setting-readahead").value+"%";
$("setting-preload-value").textContent=$("setting-preload").value+"%";
var stats=await native("/system/stats");$("server-stats").replaceChildren();[["Version",stats.version],["Uptime",stats.uptime_sec+" s"],["Active sessions",stats.stats.active_sessions],["Active readers",stats.stats.active_readers],["RAM cache",bytes(stats.stats.cache_bytes)+" / "+bytes(stats.stats.cache_capacity)],["Cached pieces",stats.stats.cache_pieces],["Disk cache",bytes(stats.stats.disk_cache_bytes)+" / "+bytes(stats.stats.disk_cache_capacity)]].forEach(function(x){var c=el("div",undefined,"stat");c.append(el("strong",String(x[1])),el("span",x[0]));$("server-stats").append(c);});}
$("server-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var v={};Object.keys(settingFields).forEach(function(k){var n=$(settingFields[k]);if(n.type==="checkbox")v[k]=n.checked;else if(n.type==="number"||n.type==="range")v[k]=Number(n.value);else v[k]=n.value;});v.CacheSize*=1048576;v.DiskCacheSize*=1073741824;var d=await native("/settings",{settings:v},"PUT");$("server-restart-note").hidden=!d.restart_required;status(d.restart_required?"Settings saved. Restart required for some changes.":"Server settings saved and applied.");});});
async function loadUsers(){var d=await native("/users"),t=$("users-rows");t.replaceChildren();(d.users||[]).forEach(function(u){var row=el("article",undefined,"row user-row"),copy=el("div",undefined,"row-copy");copy.append(el("h3",u.username+(u.id==="owner"?tr(" · Owner"):"")),el("p",tr("Rank ")+u.rank+" · "+tr(u.is_banned?"Banned":"Active"),"hint"),el("code",u.id));var actions=el("div",undefined,"user-actions");if(u.id!=="owner"&&session.rank>u.rank){var role=document.createElement("select");[[10,"User"],[50,"Administrator"]].forEach(function(x){var o=el("option",x[1]);o.value=x[0];role.append(o);});role.value=u.rank;actions.append(role,button("Set role",async function(){await native("/users/"+encodeURIComponent(u.id)+"/rank",{rank:Number(role.value)},"POST");await loadUsers();}),button(u.is_banned?"Unban":"Ban",async function(){await native("/users/"+encodeURIComponent(u.id),{banned:!u.is_banned},"PUT");await loadUsers();}),button("Reset password",async function(){var p=prompt(tr("New password for ")+u.username);if(p===null)return;if(!p)throw new Error("Password cannot be empty.");await native("/users/"+encodeURIComponent(u.id),{password:p},"PUT");status("Password changed.");}),button("Regenerate token",async function(){var x=await native("/users/"+encodeURIComponent(u.id)+"/regenerate-token",{},"POST");alert(tr("New API token (shown once):\n")+x.token);}),button("Delete",async function(){if(!confirm(tr("Delete user ")+u.username+tr(" and their private library/progress?")))return;await native("/users/"+encodeURIComponent(u.id),undefined,"DELETE");await loadUsers();},"danger"));}row.append(copy,actions);t.append(row);});}
$("user-add-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var d=await native("/users",{username:$("new-user-name").value.trim(),password:$("new-user-password").value,rank:Number($("new-user-rank").value)},"POST");$("user-add-form").reset();status(tr("Created user ")+d.username+".");await loadUsers();});});
$("library-add-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var link=$("library-link").value.trim(),file=$("library-file").files[0],posterUrl=$("library-poster-url").value.trim(),posterFile=$("library-poster-file").files[0];if(!!link===!!file)throw new Error("Choose exactly one magnet/link or one .torrent file.");if(file&&!file.size)throw new Error("The selected file is empty.");if(posterUrl&&posterFile)throw new Error("Choose either a poster URL or an image file.");if(posterFile&&!posterFile.size)throw new Error("The selected poster file is empty.");var form=new FormData();if(file)form.append("file",file,file.name);else form.append("link",link);form.append("title",$("library-title").value.trim());form.append("category",$("library-new-category").value.trim());if(posterFile)form.append("poster_file",posterFile,posterFile.name);else if(posterUrl)form.append("poster_url",posterUrl);await native("/torrents",form,"POST");$("library-add-form").reset();closeModal("add-modal");await loadLibrary();status("Torrent added to your library.");});});
$("library-import-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var f=$("library-import-file").files[0];if(!f)throw new Error("Choose a .torrs file.");var form=new FormData();form.append("file",f,f.name);var d=await native("/library/import",form,"POST");$("library-import-form").reset();await loadLibrary();status(tr("Import complete: ")+d.added+tr(" added, ")+d.skipped+tr(" already present, ")+d.rejected+tr(" rejected."));});});
$("library-filter-form").addEventListener("submit",function(e){e.preventDefault();pageOffset=0;run(loadLibrary);});$("library-reload").addEventListener("click",function(){run(loadLibrary);});$("library-prev").addEventListener("click",function(){pageOffset=Math.max(0,pageOffset-pageLimit);run(loadLibrary);});$("library-next").addEventListener("click",function(){pageOffset+=pageLimit;run(loadLibrary);});
$("detail-cover-refresh").addEventListener("click",function(){refreshDetailCover();});
$("detail-close").addEventListener("click",function(){$("torrent-detail").hidden=true;$("torrent-player").pause();$("torrent-player").removeAttribute("src");});$("files-refresh").addEventListener("click",function(){loadFiles();});$("cache-refresh").addEventListener("click",function(){loadDiagnostics();});$("peers-refresh").addEventListener("click",function(){loadDiagnostics();});
["continue-refresh","server-refresh","users-refresh","library-reload"].forEach(function(id){$(id).addEventListener("click",function(){run(id==="continue-refresh"?loadContinue:id==="server-refresh"?loadServer:id==="users-refresh"?loadUsers:loadLibrary);});});
$("search-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var d=await host.search($("query").value,$("tag").value);renderResults(d.results||[]);status((d.results||[]).length+" "+tr(" releases found."));});});
$("reload-lists").addEventListener("click",function(){run(loadLists);});$("subscribe-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){await host.subscribe($("pubkey").value.trim());status("Subscription requested.");});});
$("settings-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var token=$("daemon-token").value;$("daemon-token").value="";await host.settings({url:$("daemon-url").value.trim(),token:token,clear_token:$("clear-token").checked});await loadConnection();status("Connection saved.");});});
$("login-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){await request("/api/auth/login",{username:$("username").value.trim(),password:$("password").value});$("password").value="";await start();});});
$("logout").addEventListener("click",function(){run(async function(){await request("/api/auth/logout",{});showLogin();session=null;});});
function syncModalState(){
 var opened=!$("add-modal").hidden||!$("settings-modal").hidden;
 document.body.classList.toggle("modal-open",opened);
}
function openModal(id){$(id).hidden=false;syncModalState();}
function closeModal(id){$(id).hidden=true;syncModalState();}
function applyTheme(){
 document.documentElement.dataset.theme=theme;
 $("theme-toggle").setAttribute("aria-label",tr("Switch theme"));
 $("theme-toggle").title=tr("Switch theme");
 $("theme-toggle-settings").textContent=tr("Switch theme");
}
function localizeStatic(){
 document.documentElement.lang=language;
 document.querySelectorAll("body *").forEach(function(node){
  Array.from(node.childNodes).forEach(function(child){
   if(child.nodeType!==3||!child.nodeValue.trim())return;
   if(child.__i18nSource===undefined)child.__i18nSource=child.nodeValue.trim();
   var value=tr(child.__i18nSource),leading=child.nodeValue.match(/^\\s*/)[0],trailing=child.nodeValue.match(/\\s*$/)[0];
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
  var response=await fetch((SILO?base:"/ui")+"/lang/"+language+".json",{credentials:"same-origin",cache:"no-store"});
  if(!response.ok)throw new Error("translation file unavailable");
  locale=await response.json();
 }catch(_){locale={};}
 localizeStatic();
 applyTheme();
 $("language-select").value=language;
 $("language-select-settings").value=language;
}
async function loadPreferences(){
 var values={language:"en",theme:"light"};
 if(SILO){
  try{values=Object.assign(values,JSON.parse(localStorage.getItem("p2play.ui."+userKey)||"{}"));}catch(_){}
 }else{
  try{var data=await native("/preferences");values=Object.assign(values,data.preferences||{});}catch(_){}
 }
 language=values.language==="ru"?"ru":"en";
 theme=values.theme==="dark"?"dark":"light";
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
 status("Language saved.");
}
async function toggleTheme(){
 theme=theme==="dark"?"light":"dark";
 applyTheme();
 await savePreference("theme",theme);
 status("Theme saved.");
}
function selectCatalogResult(record){
 var link=record.torrs||(record.hash.length===64?"magnet:?xt=urn:btmh:1220"+record.hash:"magnet:?xt=urn:btih:"+record.hash);
 $("library-link").value=link;
 $("library-title").value=record.title||"";
 $("library-new-category").value=(record.tags||[])[0]||"";
 $("add-search-panel").hidden=true;
 $("add-form-panel").hidden=false;
 $("library-link").focus();
 status("Release selected. Review the details and add it to your library.");
}
async function start(){session=await host.session();userKey=String(session.user||"user");document.querySelectorAll(".silo-only").forEach(function(n){n.hidden=!SILO;});document.querySelectorAll(".core-only").forEach(function(n){n.hidden=SILO;});$("identity").hidden=false;$("identity").textContent=(session.user||"User")+(session.rank>=100?" · Owner":session.rank>=50?" · Administrator":"");$("logout").hidden=SILO;$("users-tab").hidden=SILO||session.rank<50;$("server-tab").hidden=SILO||session.rank<100;$("subscribe-form").hidden=!session.can_admin;await loadPreferences();await loadLocale();showView(SILO?"search":"library");if(!SILO)await loadLibrary();status(SILO?"Search catalogs on your daemon.":"Your library is ready.");}
$("add-open").addEventListener("click",function(){$("add-form-panel").hidden=false;$("add-search-panel").hidden=true;$("library-add-form").reset();openModal("add-modal");$("library-link").focus();});
$("add-close").addEventListener("click",function(){closeModal("add-modal");});
$("add-cancel").addEventListener("click",function(){closeModal("add-modal");});
$("add-search-open").addEventListener("click",function(){$("add-form-panel").hidden=true;$("add-search-panel").hidden=false;$("add-query").focus();});
$("add-search-close").addEventListener("click",function(){$("add-search-panel").hidden=true;$("add-form-panel").hidden=false;});
$("add-search-cancel").addEventListener("click",function(){$("add-search-panel").hidden=true;$("add-form-panel").hidden=false;});
$("add-search-form").addEventListener("submit",function(e){e.preventDefault();run(async function(){var data=await host.search($("add-query").value,$("add-tag").value);renderResults(data.results||[],"add-search-results",selectCatalogResult);});});
$("settings-open").addEventListener("click",function(){showView("preferences");});
$("settings-close").addEventListener("click",function(){showView(activeMainView);});
document.querySelectorAll("[data-settings-view]").forEach(function(tab){tab.addEventListener("click",function(){run(async function(){var view=tab.dataset.settingsView;showView(view);if(view==="lists")await loadLists();else if(view==="continue")await loadContinue();else if(view==="server")await loadServer();else if(view==="users")await loadUsers();else if(view==="settings")await loadConnection();});});});
$("theme-toggle").addEventListener("click",function(){run(toggleTheme);});
$("theme-toggle-settings").addEventListener("click",function(){run(toggleTheme);});
$("language-select").addEventListener("change",function(){run(function(){return setLanguage($("language-select").value);});});
$("language-select-settings").addEventListener("change",function(){run(function(){return setLanguage($("language-select-settings").value);});});
document.querySelectorAll(".modal-backdrop").forEach(function(modal){modal.addEventListener("click",function(e){if(e.target!==modal)return;if(modal.id==="settings-modal")showView(activeMainView);else closeModal(modal.id);});});
document.addEventListener("keydown",function(e){if(e.key!=="Escape")return;if(!$("add-modal").hidden)closeModal("add-modal");else if(!$("settings-modal").hidden)showView(activeMainView);});
$("setting-disk-cache-size").addEventListener("input",function(){$("setting-disk-cache-size-value").textContent=this.value+" GiB";});
$("setting-readahead").addEventListener("input",function(){$("setting-readahead-value").textContent=this.value+"%";});
$("setting-preload").addEventListener("input",function(){$("setting-preload-value").textContent=this.value+"%";});
document.title="MediaMesh · "+HOST;run(start);
}());
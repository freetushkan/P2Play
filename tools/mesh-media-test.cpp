// Actual Mesh integration tests. Run through mesh-media-test.py (no network needed).
#include "../src/core/mesh.hpp"
#include "../src/core/torrs.hpp"
#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/kademlia/ed25519.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <tuple>
using namespace p2p;
using nlohmann::json;
namespace fs = std::filesystem;
static int checks = 0;
static void check(bool ok, const std::string &what) {
    if (!ok) throw std::runtime_error("FAIL " + what);
    std::cout << "OK   " << what << "\n"; ++checks;
}
static std::string read(const fs::path &p) { std::ifstream in(p, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
static void write(const fs::path &p, const std::string &s) { std::ofstream out(p, std::ios::binary); out.write(s.data(), s.size()); if (!out) throw std::runtime_error("write failed"); }
static std::string hash(const std::string &s) { auto h=libtorrent::hasher256(s).final(); return std::string(h.data(), h.size()); }
static lt::entry manifest(const fs::path &dir) { auto raw=read(dir/"manifest.ben"); lt::error_code ec; auto m=lt::bdecode(raw, ec); if(ec) throw std::runtime_error("decode"); lt::entry e; e=m; return e; }
static void sign(lt::entry &e, const fs::path &root, const std::string &pk) {
    auto h=read(root/"keys"/(pk+".seed")); std::array<char,32> seed;
    auto v=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(int i=0;i<32;++i) seed[i]=static_cast<char>(v(h[2*i])*16+v(h[2*i+1]));
    lt::dht::public_key pub; lt::dht::secret_key priv; std::tie(pub,priv)=lt::dht::ed25519_create_keypair(seed);
    e.dict().erase("sig"); std::string bytes; lt::bencode(std::back_inserter(bytes),e);
    auto sig=lt::dht::ed25519_sign(bytes,pub,priv); e["sig"]=std::string(sig.bytes.data(),64);
}
static void save(const fs::path &dir, const lt::entry &e) { std::string b;lt::bencode(std::back_inserter(b),e);write(dir/"manifest.ben",b); }
static std::string mediaPath(const MeshMediaRef &r) {return "media/"+r.pack;}
static void run(const fs::path &root) {
    fs::create_directories(root/"a"); fs::create_directories(root/"b");
    Mesh a,b; auto log=[](const std::string &){}; a.load((root/"a").string(),log); b.load((root/"b").string(),log);
    std::string pk; check(a.create("Media list","test",pk).empty(),"create signing identity");
    std::string ih(40,'a'), ih2(40,'b'), dead(40,'c'), err;
    MeshMediaRef d1,d2,p1;
    std::string desc="A UTF-8 description: описания, ёлки.";
    std::string png("\x89PNG\r\n\x1a\n",8); png+=std::string(128,'P');
    check(a.addMedia(pk,"description","text/plain; charset=utf-8",desc,d1).empty(),"append description media");
    check(a.addMedia(pk,"description","text/plain; charset=utf-8","Second description",d2).empty() && d1.pack==d2.pack && d2.offset==d1.length,"pack append preserves previous slice offsets");
    check(a.addMedia(pk,"poster","image/png",png,p1).empty() && p1.pack!=d1.pack,"poster pack separate from description pack");
    check(!a.addMedia(pk,"poster","image/svg+xml","<svg/>",p1).empty(),"active poster MIME rejected");
    check(!a.addMedia(pk,"description","text/plain; charset=utf-8",std::string("\xc0\x80",2),p1).empty(),"invalid UTF-8 rejected");
    check(!a.addMedia(pk,"description","text/plain; charset=utf-8",std::string(Mesh::kMaxDescriptionBytes+1,'X'),p1).empty(),"description hard bound");
    // Invalid upload must leave the previously returned reference untouched.
    json r1={{"ih",ih},{"title","Alpha"},{"shard","movies"},{"description_ref",d1.json()},{"poster_ref",p1.json()}};
    json r2={{"ih",ih2},{"title","Beta"},{"shard","series"},{"description_ref",d2.json()}};
    check(a.addPending(pk,r1.dump()+"\n"+r2.dump()+"\n"+json{{"ih",dead},{"title","Deleted"},{"shard","movies"}}.dump(),err)==3,"records carry signed media references and shards");
    int64_t seq=0; check(a.release(pk,seq).empty() && seq==1,"release signed pack descriptors");
    const auto src=fs::path(a.listDir(pk)); auto v1=manifest(src);
    auto firstPack=read(src/mediaPath(d1));
    std::string bytes,type;
    check(a.media(pk,ih,"description",bytes,type).empty() && bytes==desc,"lazy slice with full pack and object SHA-256 verification");
    check(a.media(pk,ih,"poster",bytes,type).empty() && bytes==png && type=="image/png","lazy poster bytes and safe MIME");
    MeshRecord initial; check(a.record(ih,initial),"record accessible");
    // All new uploads after a release must go to a different immutable pack.
    MeshMediaRef d3; check(a.addMedia(pk,"description","text/plain; charset=utf-8","Replacement",d3).empty() && d3.pack!=d1.pack,"new version never appends a published pack");
    r1["description_ref"]=d3.json();
    check(a.addPending(pk,r1.dump()+"\n"+json{{"del",dead},{"shard","movies"}}.dump(),err)==2,"queue update and shard tombstone");
    check(a.release(pk,seq).empty() && seq==2 && read(src/mediaPath(d1))==firstPack,"append release leaves old pack byte-for-byte unchanged");
    check(b.accept(src.string()).empty(),"another node imports checked shards and packs");
    MeshRecord rr;
    check(!b.record(dead,rr) && b.record(ih,rr),"latest shard tombstones respected");
    auto policy=b.policy(pk); policy.shards={"movies"}; policy.mediaBytes=0; policy.cacheBytes=0;
    check(b.setPolicy(pk,policy).empty(),"persist shard selection and zero media/cache budget");
    check(b.record(ih,rr) && !b.record(ih2,rr),"only selected shard indexed");
    const auto dest=fs::path(b.listDir(pk));
    check(fs::is_empty(dest/"media") && b.media(pk,ih,"description",bytes,type)=="media unavailable","media lazily absent, zero disk budget enforced");
    check(b.installFile(pk,mediaPath(d3),(src/mediaPath(d3)).string())=="media disk budget exceeded","pack install respects disk quota");
    policy.mediaBytes=Mesh::kMaxPackBytes; policy.cacheBytes=32;
    check(b.setPolicy(pk,policy).empty() && b.installFile(pk,mediaPath(d3),(src/mediaPath(d3)).string()).empty(),"install authenticated requested pack after budget increase");
    check(b.media(pk,ih,"description",bytes,type).empty() && bytes=="Replacement","lazy media read after pack install");
    const auto cache=root/"b"/"mesh-local"/pk/"cache"/d3.sha256;
    check(fs::exists(cache),"content-addressed SHA-256 cache populated");
    write(cache,"poison");
    check(b.media(pk,ih,"description",bytes,type).empty() && bytes=="Replacement" && read(cache)=="Replacement","poisoned cache detected and repaired");
    fs::remove(cache); auto good=read(dest/mediaPath(d3));auto corrupt=good;corrupt[0]^=1;write(dest/mediaPath(d3),corrupt);
    check(b.media(pk,ih,"description",bytes,type)=="media pack hash mismatch" && bytes.empty(),"tampered pack refused before serving");
    write(dest/mediaPath(d3),good);
    check(b.installFile(pk,mediaPath(p1),(src/mediaPath(p1)).string()).empty() && b.media(pk,ih,"poster",bytes,type).empty(),"read object larger than cache quota without caching it");
    auto cacheDir=root/"b"/"mesh-local"/pk/"cache";
    check(!fs::exists(cacheDir/p1.sha256),"single object cannot exceed cache budget");
    policy.recordBytes=0;check(b.setPolicy(pk,policy).empty() && b.search("","",100).empty(),"zero record budget drops complete shards, not arbitrary deltas");
    policy.recordBytes=256ll<<20;policy.shards={"*"};check(b.setPolicy(pk,policy).empty(),"reselect missing shards");
    check(b.search("","",100).empty(),"incomplete shard histories do not resurrect deleted records");
    auto files=b.plan(pk);
    for(const auto &f:files) if(f.kind=="records" && f.selected) check(b.installFile(pk,f.path,(src/f.path).string()).empty(),"install selected signed shard "+f.shard);
    check(b.record(ih2,rr) && !b.record(dead,rr),"shard indexes rebuilt only after complete history installed");
    // Quota admission is whole-shard: one byte less cannot admit a prefix.
    int64_t movieBytes=0;for(const auto &f:b.plan(pk))if(f.shard=="movies")movieBytes+=f.bytes;
    policy.shards={"movies"};policy.recordBytes=movieBytes-1;
    check(b.setPolicy(pk,policy).empty() && b.search("","",100).empty(),"whole-shard disk budget admission");
    check(!b.setPolicy(pk,MeshPolicy{{"../escape"},100,100,100}).empty(),"unsafe shard names rejected");
    // Snapshot the owner, reclaim tombstones/old segments and unreferenced packs.
    auto before=manifest(src); auto beforeCount=before["segs"].list().size();
    int64_t beforeMedia=0;for(const auto &f:a.plan(pk))if(f.kind!="records")beforeMedia+=f.bytes;
    check(a.compact(pk,seq).empty() && seq==3,"owner snapshot signed at a higher sequence");
    auto after=manifest(src);
    int64_t afterMedia=0;for(const auto &f:a.plan(pk))if(f.kind!="records")afterMedia+=f.bytes;
    check(afterMedia<beforeMedia,"snapshot reclaims unreferenced bytes inside shared packs");
    check(after["segs"].list().size()<beforeCount && after["snapshot"].integer()==2 && !a.record(dead,rr),"snapshot compacts live state and records ancestry");
    check(!fs::exists(src/mediaPath(d1)) && !fs::exists(src/"records"/"seg-000001.z"),"snapshot repacks live slices and removes old packs and record history");
    Mesh reloaded;reloaded.load((root/"a").string(),log);
    check(reloaded.record(ih2,rr) && reloaded.media(pk,ih,"description",bytes,type).empty() && bytes=="Replacement","snapshot reload preserves records and media refs");
    check(b.accept(src.string()).empty(),"new snapshot accepted with subscriber policy");
    check(!b.accept(src.string()).empty(),"same sequence cannot roll back or replay");
    // Restore default selection and install full snapshot.
    policy=MeshPolicy();check(b.setPolicy(pk,policy).empty(),"restore defaults");
    for(const auto &f:b.plan(pk))if(f.kind=="records")check(b.installFile(pk,f.path,(src/f.path).string()).empty(),"install compacted shard");
    Mesh bRestart;bRestart.load((root/"b").string(),log);
    check(bRestart.record(ih,rr) && bRestart.policy(pk).shards.count("*"),"selection, budget and snapshot persist across restart");
    // Signature, descriptor path traversal, offset overflow, count and hash adversarial cases.
    const auto bad=root/"bad";
    auto clone=[&]{fs::remove_all(bad);fs::copy(src,bad,fs::copy_options::recursive);};
    clone();auto e=manifest(bad);e["name"]="tampered";save(bad,e);
    check(b.accept(bad.string())=="bad signature","manifest mutation cannot forge pack descriptors");
    clone();e=manifest(bad);e["seq"]=10;e["packs"].list()[0]["f"]="../../escape";sign(e,root/"a",pk);save(bad,e);
    check(b.accept(bad.string())=="bad pack entry","even a signed unsafe pack path refused");
    clone();e=manifest(bad);e["seq"]=10;e["segs"].list()[0]["n"]=999;sign(e,root/"a",pk);save(bad,e);
    check(b.accept(bad.string())=="segment record count mismatch","signed record counts checked against payload");
    clone();e=manifest(bad);e["seq"]=10;auto &seg=e["segs"].list()[0];auto segment=bad/"records"/seg["f"].string();
    std::string text;check(zlibInflate(read(segment),text,Mesh::kMaxVersionTextBytes),"decode test segment");
    auto end=text.find('\n');auto j=json::parse(text.substr(0,end));j["description_ref"]["offset"]=INT64_MAX;
    auto z=zlibDeflate(j.dump()+"\n"+text.substr(end+1));write(segment,z);seg["h"]=hash(z);seg["b"]=static_cast<int64_t>(z.size());sign(e,root/"a",pk);save(bad,e);
    check(b.accept(bad.string()).find("bad description_ref")!=std::string::npos,"overflowing media slice offset rejected");
    // Descriptors-only import is useful for network metadata-first fetches.
    clone();fs::remove_all(bad/"records");fs::create_directories(bad/"records");fs::remove_all(bad/"media");fs::create_directories(bad/"media");
    fs::create_directories(root/"c");Mesh c;c.load((root/"c").string(),log);
    check(c.accept(bad.string(),std::string(64,'0'),3)=="pointer/manifest key or version mismatch","network import pins expected manifest key");
    check(c.accept(bad.string(),pk,4)=="pointer/manifest key or version mismatch","network import pins expected manifest sequence");
    check(c.lists().empty(),"mismatched pointer identity does not commit a catalog");
    check(c.accept(bad.string(),pk,3).empty() && c.search("","",10).empty(),"signed descriptors accepted without eager segment/media downloads");
    auto f=c.plan(pk).front();auto damaged=root/"damaged";write(damaged,"not a segment");
    check(c.installFile(pk,f.path,damaged.string())=="file hash mismatch","downloaded shard verified before installation");
    check(c.installFile(pk,"../manifest.ben",damaged.string())=="not a signed version file","install path constrained to signed descriptors");
    clone(); e=manifest(bad); e["seq"]=10; auto &signedSeg=e["segs"].list()[0]; auto signedPath=bad/"records"/signedSeg["f"].string();
    check(zlibInflate(read(signedPath),text,Mesh::kMaxVersionTextBytes),"decode forged-slice test segment");
    end=text.find('\n');j=json::parse(text.substr(0,end));j["description_ref"]["sha256"]=std::string(64,'0');
    z=zlibDeflate(j.dump()+"\n"+text.substr(end+1)); write(signedPath,z); signedSeg["h"]=hash(z); signedSeg["b"]=static_cast<int64_t>(z.size());sign(e,root/"a",pk);save(bad,e);
    check(c.accept(bad.string()).empty(),"lazy import does not eagerly scan media objects");
    check(c.media(pk,ih,"description",bytes,type)=="media hash or type mismatch" && bytes.empty(),"object SHA-256 enforced even for correctly signed intact pack");
    check(a.addPending(pk,json{{"ih",ih},{"title","Alpha"},{"description_ref",{{"pack",d3.pack},{"offset",0},{"length",12},{"sha256",std::string(64,'0')},{"type","text/plain; charset=utf-8"}}}}.dump(),err)==1,"syntactically valid wrong content hash queued");
    auto held=read(src/"manifest.ben");
    check(!a.release(pk,seq).empty() && read(src/"manifest.ben")==held && a.pending(pk)==1,"failed publish leaves committed manifest and pending work intact");
    check(!a.compact(pk,seq).empty(),"snapshot cannot discard unreleased pending work");

    // Meta-lists are signed one-level references, not catalogs with another record payload.
    std::string metaPk, nestedPk, maliciousPk;
    check(a.create("Curated collection","signed references",metaPk,true).empty(),"create an explicit meta-list");
    check(a.create("Nested collection","must remain a root",nestedPk,true).empty(),"create standalone nested meta-list");
    check(a.setMetaItems(metaPk,{{pk,"Feature films",true}}).empty(),"stage a reference to a held ordinary catalog");
    int64_t metaSeq=0;
    check(a.release(metaPk,metaSeq).empty() && metaSeq==1,"metadata-only release signs meta items");
    MeshList metaCard;
    check(a.list(metaPk,metaCard) && metaCard.meta && metaCard.items.size()==1 &&
          metaCard.items[0].pubkey==pk && metaCard.items[0].annotation=="Feature films" && metaCard.items[0].enabled,
          "signed meta items round-trip through manifest verification");
    check(!a.setMetaItems(metaPk,{{metaPk,"self",true}}).empty(),"publisher refuses self-reference");
    check(!a.setMetaItems(metaPk,{{std::string(64,'f'),"unknown",true}}).empty(),"publisher refuses unknown child key");
    check(!a.setMetaItems(metaPk,{{nestedPk,"nested",true}}).empty(),"publisher refuses nested meta-list reference");
    check(!a.setMetaItems(metaPk,{{pk,"first",true},{pk,"duplicate",false}}).empty(),"publisher refuses duplicate child keys");
    check(a.addPending(metaPk,json{{"ih",ih},{"title","not a catalog record"}}.dump(),err)<0,
          "meta-list cannot accept catalog records");
    check(a.addMedia(metaPk,"description","text/plain; charset=utf-8","not a media pack",d1).find("meta-lists")!=std::string::npos,
          "meta-list cannot accept media packs");

    // Even a correctly signed hostile manifest cannot point at a meta-list already held locally.
    check(a.create("Hostile parent","test receiver depth guard",maliciousPk,true).empty(),"create parent for adversarial manifest");
    auto hostile=manifest(fs::path(a.listDir(maliciousPk)));
    hostile["seq"]=1;
    std::string childRaw(32,'\\0');
    auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c-'a'+10;};
    for(int i=0;i<32;++i) childRaw[i]=static_cast<char>((nibble(metaPk[2*i])<<4)|nibble(metaPk[2*i+1]));
    lt::entry child(lt::entry::dictionary_t); child["pk"]=childRaw; child["note"]="nested"; child["enabled"]=1;
    lt::entry::list_type children; children.push_back(child); hostile["meta"]=children;
    sign(hostile,root/"a",maliciousPk); save(fs::path(a.listDir(maliciousPk)),hostile);
    Mesh c2; c2.load((root/"c2").string(),log);
    check(c2.accept(a.listDir(metaPk),metaPk,metaSeq).empty(),"subscriber can hold a meta-list as a root");
    check(c2.accept(a.listDir(maliciousPk),maliciousPk,1)=="meta-list cannot include another meta-list",
          "receiver rejects signed reference to an already-held meta-list");
    Mesh childGuard; childGuard.load((root/"child-guard").string(),log);
    check(childGuard.accept(a.listDir(metaPk),metaPk,metaSeq,true)=="nested meta-lists are not allowed",
          "network child acceptance refuses meta-lists before commit");

    check(!a.remove(pk).empty() && bRestart.remove(pk).empty(),"owner protected; received catalog and local media removed");
    std::cout<<"PASS "<<checks<<" checks\n";
}
int main(int argc,char **argv){try{if(argc!=2)throw std::runtime_error("scratch directory required");run(fs::path(argv[1]));return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}

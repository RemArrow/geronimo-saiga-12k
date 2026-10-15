// Saiga-12K - GML plugin (com.remarrow.saiga12). Personal use: the model and textures come from the
// user's own export of Escape from Tarkov and must not be distributed.
//
// Adds a "Saiga-12K" to the Shotgun spawner: a full-auto 12 gauge firing slugs, built on the game's
// AK-74M (same action: side charging handle, rock-in magazine, safe/auto/semi lever) WITHOUT replacing
// any game asset - every other AK-74M stays an AK-74M.
//
// 1. Spawner entry: after BP_GameInstance::GetItems builds AvailableGuns, a GunAsset_C constructed at
//    runtime (the AK-74M's receiver, an icon rendered from the Saiga model) is appended to the Shotgun list.
// 2. Visuals: an AK-74M receiver spawned from that entry (or rebuilt from a saved loadout slot holding it)
//    gets the Saiga as DynamicMeshComponents built from the OBJs embedded in this DLL, textured with
//    M_DefaultShader instances; the AK-74M mesh and moving parts are hidden. Its own bolt carrier with the
//    charging handle, bolt, fire selector, trigger and magazine release move on their own pivots, posed by
//    this plugin every frame from the gun's state ("the Saiga's own action"). Magazines in a Saiga, and
//    the spares its holder carries, become the 5-round Sb.5: its mesh, 5 rounds, 12 gauge.
//    The Saiga is fitted on the AK-74M by its pistol grip (the same AK-74 grip on both guns, tools\prep),
//    and the muzzle point and side-rail mount are moved onto the Saiga's.
// 3. 12 gauge: the gun fires the game's 12 gauge round (shells in the chamber and out of the ejection
//    port), at the Benelli M4's 450 rpm and recoil, with the Benelli's shot sound, in full auto.
// 4. Slugs: the game only has 9-pellet buckshot. AGunReceiver::FireRoundLocal (native) spawns the round
//    from its projectile class's defaults, so while a Saiga fires, that class's defaults are a slug
//    (one 28 g projectile, its own damage curve, never recycled from the bullet pool) and are put back
//    the moment the shot is out. Every other 12 gauge gun keeps its buckshot.
// 5. The Shotguns category: the gun station's menu has one, wired up but shipped hidden. It is shown, with
//    the Saiga in it; the game's unreleased Benelli M4 ("M1014") is taken out of that list.
//
// Settings: GML\config\com.remarrow.saiga12.cfg. Build: build.bat [install].
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include "payload.h"
#include "payload_key.h"  // generated per build by tools\pack (build\gen), never committed
#include "version.h"

using namespace gml;

static constexpr uint8_t kShotgun = 3;  // EGunType
static const char* kDonor = "/Game/Weapons/Data/DA_AK74M.DA_AK74M";
static const char* kDisplayName = "Saiga-12K";
static const char* kRound = "/Game/Weapons/Ammunition/12Gauge/BP_GunProjectile_12G_Buck.BP_GunProjectile_12G_Buck_C";
static const char* kShell = "/Game/Weapons/Ammunition/12Gauge/12G_Shell.12G_Shell";
static const char* kShellUsed = "/Game/Weapons/Ammunition/12Gauge/12G_Shell_Used.12G_Shell_Used";
static const char* kShotgunGun = "/Game/Weapons/BenelliM4/BenelliM4.BenelliM4_C";  // the game's own 12 gauge: rate, recoil
static const char* kShotgunSfx = "/Game/Weapons/BenelliM4/Data/GunSFX_BenelliM4.GunSFX_BenelliM4";

static GUObject* s_gun = nullptr;  // our runtime GunAsset

// ------------------------------------------------------------------ assets

// Decrypts one asset from the payload into `out`. Callers wipe it as soon as the engine has it, so at
// most one asset exists in plain form, and only for that moment.
static bool Unpack(const char* name, std::vector<uint8_t>& out, bool optional = false) {
    static Blob blob = Resource("DATA");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = kPayloadKeyA[i] ^ kPayloadKeyB[i];
    bool ok = blob && payload::Extract(blob.data, blob.size, key, name, out);
    SecureZeroMemory(key, sizeof key);
    if (!ok && !optional) Error("embedded asset '{}' is missing or damaged", name);
    return ok;
}

static GUObject* Icon() {
    static GUObject* icon = nullptr;
    if (icon && API->IsValid(icon)) return icon;
    std::vector<uint8_t> buf;
    if (Unpack("Icon", buf, true)) {
        icon = API->ImportTextureFromMemory(buf.data(), buf.size(), "SaigaIcon");
        payload::Wipe(buf);
        if (icon) API->KeepAlive(icon);
    }
    return icon;
}

// ------------------------------------------------------------------ spawner entry

static GML_TArray* GunList(Object gi, uint8_t category) {
    GFProperty* mapProp = API->FindProperty(gi.Struct(), "AvailableGuns");
    void* map = gi.PropPtr("AvailableGuns");
    if (!mapProp || !map) return nullptr;
    struct Ctx { GML_TArray* found; GUStruct* valueStruct; uint8_t category; } ctx{nullptr, nullptr, category};
    GML_PropInfo mi, vi;
    API->GetPropertyInfo(mapProp, &mi);
    API->GetPropertyInfo(mi.value, &vi);
    ctx.valueStruct = vi.structType;  // FGunAssetList
    API->MapForEach(map, mapProp, [](void* u, void* key, void* value) -> int {
        auto* c = (Ctx*)u;
        if (*(uint8_t*)key != c->category) return 0;
        GFProperty* arr = API->FindProperty(c->valueStruct, "GunAssets");  // GUID-mangled in the BP struct
        GML_PropInfo ai;
        if (!arr || !API->GetPropertyInfo(arr, &ai)) return 1;
        c->found = (GML_TArray*)((uint8_t*)value + ai.offset);
        return 1;
    }, &ctx);
    return ctx.found;
}

static void AddGun(Object gi) {
    GML_TArray* raw = GunList(gi, kShotgun);
    if (!raw) { Warn("AvailableGuns[Shotgun] not found yet"); return; }
    TArrayView<GUObject*> list{raw};
    if (s_gun && Object(s_gun) && list.Contains(s_gun)) return;  // GetItems ran again; already there

    Object donor = FindObject(kDonor);
    GUClass* gunClass = FindClass("GunAsset_C");
    if (!donor || !gunClass) { Error("donor {} or GunAsset_C missing", kDonor); return; }

    Object gun = NewObject(gunClass, gi);
    if (!gun) { Error("could not construct GunAsset_C"); return; }
    gun.Set("ReceiverClass", donor.Get<GUClass*>("ReceiverClass"));
    GUObject* icon = Icon();
    gun.Set("Icon", icon ? icon : donor.Get<GUObject*>("Icon"));
    gun.SetBool("bEnabled", donor.GetBool("bEnabled"));
    gun.Set("Category", kShotgun);
    uint8_t text[16];
    if (API->MakeText(kDisplayName, text)) std::memcpy(gun.PropPtr("DisplayName"), text, 16);

    int before = list.Num();
    if (list.Add(gun.ptr)) {
        s_gun = gun.ptr;
        Log("*** ADDED '{}' to the Shotgun spawner ({} -> {}) as {}{} ***", kDisplayName, before, list.Num(), gun.FullName(),
            icon ? "" : " (AK-74M icon: the Saiga's could not be built)");
    } else {
        Error("append failed");
    }
}

// ------------------------------------------------------------------ the Shotguns category
//
// The gun station's menu (WB_GunCustomization) has a Shotguns category button, CategoryBtn_Shotguns, wired
// to ShowWeapons(Shotgun) like the Assault Rifles / SMGs / Snipers / Pistols buttons, but the game ships it
// Hidden (its layout slot under Pistols stays reserved), so its list can't be reached. Nothing in the
// menu's script changes its visibility, so it is made Visible on every menu that exists.

static bool s_shotgunCategory = true;

static void ShowShotgunCategory() {
    static ULONGLONG next = 0;
    if (!s_shotgunCategory || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    for (auto& menu : FindAllOf("WB_GunCustomization_C")) {
        Object button = menu.GetObj("CategoryBtn_Shotguns");
        if (!button) continue;
        Params gv(button, "GetVisibility");
        gv.Call();
        if (gv.Return<uint8_t>() == 0) continue;  // ESlateVisibility::Visible
        Params sv(button, "SetVisibility");
        sv.Set("InVisibility", (uint8_t)0).Call();
        Log("{}: Shotguns category shown", menu.Name());
    }
}

// The game's own shotgun list also holds its unreleased Benelli M4 ("M1014", BenelliM4_C). It stays
// unreleased: it is taken out of the list, so the category holds only the Saiga.
static void RemoveBenelli(Object gi) {
    GML_TArray* raw = GunList(gi, kShotgun);
    if (!raw || !raw->Data) return;
    auto** guns = (GUObject**)raw->Data;
    int kept = 0;
    for (int i = 0; i < raw->Num; i++) {
        Object gun(guns[i]);
        if (gun && Object((GUObject*)gun.Get<GUClass*>("ReceiverClass")).Name() == "BenelliM4_C") {
            Log("{} (the Benelli M4) taken out of the Shotgun spawner", gun.Name());
            continue;
        }
        guns[kept++] = guns[i];
    }
    raw->Num = kept;
}

// ------------------------------------------------------------------ visuals

static const char* kShader = "/Game/Art/Common/M_DefaultShader.M_DefaultShader";
// Texture sets / material slots = OBJ `usemtl` groups (tools\prep).
static const char* kParts[] = {"Body", "Bolt", "Handguard", "Grip", "Cover", "Stock", "Muzzle", "Mag"};
static constexpr int kNumParts = 8;

// The Saiga's own moving parts (see "the Saiga's own action" below). `bone` is the AK-74M movables bone
// a part rides only if the plugin can't pose it itself.
enum class PartKind { Carrier, BoltHead, Selector, Trigger, MagRelease };
struct MovingPart { const char* asset; PartKind kind; const char* bone; GUObject* mesh; };
static MovingPart s_movingParts[] = {
    {"saiga_bolt", PartKind::Carrier, "RootBoltCarrier", nullptr},    // bolt carrier with its charging handle
    {"saiga_boltface", PartKind::BoltHead, "RootBoltCarrier", nullptr},  // the bolt
    {"saiga_selector", PartKind::Selector, "RootSelector", nullptr},
    {"saiga_trigger", PartKind::Trigger, "RootTrigger", nullptr},
    {"saiga_magrelease", PartKind::MagRelease, "RootMagRelease", nullptr},
};
static constexpr int kNumMoving = (int)std::size(s_movingParts);

static bool s_ownMovingParts = true, s_ownMagazines = true, s_fitAttachments = true;
static float s_offset[3] = {0, 0, 0};  // cm, a nudge on top of the fit baked into the OBJs
static GUObject* s_mesh = nullptr;        // runtime UDynamicMesh: body, handguard, grip, dust cover, stock
static GUObject* s_muzzleMesh = nullptr;  // muzzle brake (hidden while a muzzle device is mounted)
static GUObject* s_magMesh = nullptr;     // the Sb.5, in gun space as if seated
static GUObject* s_mids[kNumParts] = {};  // one M_DefaultShader instance per texture set
static bool s_assetsTried = false;
static GUClass* s_receiverClass = nullptr;

struct Pending { GUObject* actor; ULONGLONG since; };
static std::vector<Pending> s_pending;

static bool BuildVisualAssets() {
    if (s_assetsTried) return s_mesh != nullptr;
    s_assetsTried = true;
    if (API->size < offsetof(GML_API, ImportDynamicMeshFromMemory) + sizeof(void*)) {
        Error("this plugin needs GML 2.2 or newer (assets embedded in the DLL)");
        return false;
    }
    Object shader = LoadObject(kShader);
    if (!shader) { Error("weapon shader {} not found", kShader); return false; }

    std::vector<uint8_t> buf;
    for (int i = 0; i < kNumParts; i++) {
        s_mids[i] = API->CreateMaterialInstance(shader.ptr);
        if (!s_mids[i]) { Error("could not create a material instance for {}", kParts[i]); return false; }
        for (const char* param : {"Diffuse", "Normal", "ORM"}) {
            std::string res = std::string(kParts[i]) + "_" + param;
            GUObject* tex = Unpack(res.c_str(), buf) ? API->ImportTextureFromMemory(buf.data(), buf.size(), res.c_str()) : nullptr;
            payload::Wipe(buf);
            if (!tex || !API->SetMaterialTexture(s_mids[i], param, tex))
                Warn("{}: texture {} missing - that channel keeps the shader default", kParts[i], param);
        }
    }

    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_UNREAL;  // tools\prep writes the game's space, cm, fitted
    std::memcpy(o.offset, s_offset, sizeof s_offset);
    o.flipWinding = -1;
    o.materialCount = kNumParts;
    o.materialNames = kParts;
    o.materials = s_mids;
    auto load = [&](const char* name, bool optional = false) -> GUObject* {
        if (!Unpack(name, buf, optional)) return nullptr;
        GUObject* m = API->ImportDynamicMeshFromMemory(buf.data(), buf.size(), name, &o);
        payload::Wipe(buf);
        if (!m) Error("could not build the Saiga mesh '{}'", name);
        return m;
    };
    s_mesh = load("saiga");
    s_muzzleMesh = load("saiga_muzzle");
    s_magMesh = load("saiga_mag");
    for (auto& p : s_movingParts) p.mesh = load(p.asset, true);  // a missing part stays the AK-74M's bone, unseen
    if (s_mesh) Log("Saiga visuals ready: {}", Object(s_mesh).FullName());
    return s_mesh != nullptr;
}

// ------------------------------------------------------------------ transforms (raw FTransform bytes)

using Xf = std::vector<uint8_t>;
static size_t XfSize() {
    static size_t size = 0;
    if (!size) {
        GML_PropInfo ri;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &ri);
        size = ri.size;
    }
    return size;
}
static Xf TakeXf(Params& p, const char* name = "ReturnValue") {
    auto* s = (uint8_t*)p.Ptr(name);
    return Xf(s, s + XfSize());
}
static void PutXf(Params& p, const char* name, const Xf& x) { std::memcpy(p.Ptr(name), x.data(), x.size()); }
static Xf XfCall(const char* fn, std::initializer_list<std::pair<const char*, const Xf*>> args) {
    Params p(Lib("KismetMathLibrary"), fn);
    for (auto& [n, x] : args) PutXf(p, n, *x);
    p.Call();
    return TakeXf(p);
}
static Xf Compose(const Xf& a, const Xf& b) { return XfCall("ComposeTransforms", {{"A", &a}, {"B", &b}}); }  // a then b
static Xf Invert(const Xf& t) { return XfCall("InvertTransform", {{"T", &t}}); }
static Xf RelativeTo(const Xf& a, const Xf& to) { return XfCall("MakeRelativeTransform", {{"A", &a}, {"RelativeTo", &to}}); }
static Xf WorldXf(Object obj) {  // actor or scene component
    Params p(obj, obj.IsA("Actor") ? "GetTransform" : "K2_GetComponentToWorld");
    p.Call();
    return TakeXf(p);
}
static void SetRelativeXf(Object comp, const Xf& x) {
    Params p(comp, "K2_SetRelativeTransform");
    PutXf(p, "NewTransform", x);
    p.Set("bSweep", false).Set("bTeleport", true).Call();
}
static void AttachTo(Object comp, Object parent, const char* socket) {
    Params at(comp, "K2_AttachToComponent");
    at.SetObj("Parent", parent).Set("SocketName", socket ? API->MakeName(socket) : uint64_t(0))
        .Set("LocationRule", (uint8_t)2).Set("RotationRule", (uint8_t)2).Set("ScaleRule", (uint8_t)2)  // SnapToTarget
        .Set("bWeldSimulatedBodies", false).Call();
}

// A bone's reference-pose transform in component space: its local ref transforms composed up the
// parent chain. The rest pose, whatever the gun is doing right now (bolt back, trigger held).
static Xf RefPoseComponent(Object skel, const char* bone) {
    Xf acc;
    uint64_t name = API->MakeName(bone);
    for (int guard = 0; guard < 64 && name; guard++) {
        Params bi(skel, "GetBoneIndex");
        *(uint64_t*)bi.Ptr("BoneName") = name;
        bi.Call();
        int idx = bi.Return<int32_t>();
        if (idx < 0) break;
        Params rp(skel, "GetRefPoseTransform");
        rp.Set("BoneIndex", (int32_t)idx).Call();
        Xf local = TakeXf(rp);
        acc = acc.empty() ? local : Compose(acc, local);  // child first, then its parent
        Params pb(skel, "GetParentBone");
        *(uint64_t*)pb.Ptr("BoneName") = name;
        pb.Call();
        name = pb.Return<uint64_t>();
    }
    return acc;
}

static void HideComponent(Object c) {
    Params v(c, "SetVisibility");
    v.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();  // attached children stay visible
}

struct Vec3 { double x, y, z; };

// Where attachments go: the AK-74M receiver's muzzle point (shot origin and muzzle-device mount) and its
// side-rail mount, moved onto the Saiga's barrel end and side rail (actor space, cm, tools\prep's fit).
struct AttachPoint { const char* name; double x, y, z; };
static const AttachPoint kAttachPoints[] = {
    {"Fire Location", 49.490, 0.0, -0.278},   // the AK-74M's brake starts at its point, the Saiga's here
    {"AK_Dovetail", -4.282, -2.527, -1.965},  // the Saiga's side rail (tools\prep: mod_mount_000)
};
static const Vec3 kAkAttachPoints[] = {{47.639, 0, 0.068}, {-4.245, -2.576, -0.967}};  // the same points on the AK-74M

static Vec3 AttachTarget(const AttachPoint& p) { return {p.x + s_offset[0], p.y + s_offset[1], p.z + s_offset[2]}; }

// Actor transform <-> world, via KismetMathLibrary on the actor's FTransform.
struct ActorFrame {
    Params gt;
    size_t size;
    explicit ActorFrame(Object actor) : gt(actor, "GetTransform") {
        gt.Call();
        GML_PropInfo xf;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &xf);
        size = xf.size;
    }
    Vec3 Call(const char* fn, const char* arg, Vec3 v) {
        Params p(Lib("KismetMathLibrary"), fn);
        std::memcpy(p.Ptr("T"), gt.Ptr("ReturnValue"), size);
        p.Set(arg, v).Call();
        return p.Return<Vec3>();
    }
    Vec3 ToWorld(Vec3 local) { return Call("TransformLocation", "Location", local); }
    Vec3 ToLocal(Vec3 world) { return Call("InverseTransformLocation", "Location", world); }
    Vec3 DirToWorld(Vec3 local) { return Call("TransformDirection", "Direction", local); }
};

static const AttachPoint* FindAttachPoint(const std::string& componentName) {
    for (auto& p : kAttachPoints)
        if (componentName == p.name) return &p;
    return nullptr;
}

// Attachments already on the gun (a loadout restores them from their saved transforms) move with the
// point they hang on: a muzzle device onto the barrel end, a side mount onto the Saiga's rail.
static int SnapMountedAttachments(Object gun, ActorFrame& frame) {
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
    auto* arr = (GML_TArray*)ga.Ptr("OutActors");
    int snapped = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object att(((GUObject**)arr->Data)[i]);
        if (!att.IsA("GunAttachment")) continue;
        Object ap = att.GetObj("AttachPoint");
        if (!ap) continue;
        Params loc(ap, "K2_GetComponentLocation");
        loc.Call();
        Vec3 now = frame.ToLocal(loc.Return<Vec3>());
        const AttachPoint* point = nullptr;
        for (Object c : {Object(att.GetObj("RootComponent").GetObj("AttachParent")), att.GetObj("AttachablePoint")})
            if (!point && c) point = FindAttachPoint(c.Name());
        for (size_t k = 0; !point && k < std::size(kAttachPoints); k++) {  // restored onto the gun body: the AK-74M point it sat on
            const Vec3& ak = kAkAttachPoints[k];
            if (std::hypot(ak.x - now.x, ak.y - now.y, ak.z - now.z) < 1.5) point = &kAttachPoints[k];
        }
        if (!point) continue;
        Vec3 want = AttachTarget(*point);
        if (std::fabs(want.x - now.x) + std::fabs(want.y - now.y) + std::fabs(want.z - now.z) < 0.001) continue;
        Vec3 d = frame.DirToWorld({want.x - now.x, want.y - now.y, want.z - now.z});
        Params mv(att, "K2_AddActorWorldOffset");
        mv.Set("DeltaLocation", d).Set("bSweep", false).Set("bTeleport", true).Call();
        snapped++;
    }
    return snapped;
}

static void FitAttachPoints(Object actor) {
    ActorFrame frame(actor);
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    int moved = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (const AttachPoint* p = FindAttachPoint(c.Name())) {
            Params sw(c, "K2_SetWorldLocation");
            sw.Set("NewLocation", frame.ToWorld(AttachTarget(*p))).Set("bSweep", false).Set("bTeleport", true).Call();
            moved++;
        }
    }
    if (moved != (int)std::size(kAttachPoints))
        Warn("{}: moved {} of {} attach points - receiver layout differs from the AK-74M's", actor.Name(), moved, std::size(kAttachPoints));
    if (int n = SnapMountedAttachments(actor, frame)) Log("{}: {} mounted attachment(s) moved onto the Saiga", actor.Name(), n);
}

// ------------------------------------------------------------------ 12 gauge

static int s_capacity = 5, s_fireRate = 450;
static constexpr float kCarrierTravel = -12;   // cm, the charging handle's travel: the Saiga carrier's room
static constexpr float kSafeStop = 0.511f;     // of the travel: on safe the handle stops at the lever (6.13 cm)
static float s_recoilScale = 1.0f;

static GUClass* RoundClass() {
    static GUClass* cls = nullptr;
    if (!cls) cls = LoadClass(kRound);
    return cls;
}

// The AK-74M's sound set with the Benelli M4's 12 gauge shot and tail, as a runtime asset.
static GUObject* SaigaAudio(Object base) {
    static GUObject* audio = nullptr;
    if (audio && API->IsValid(audio)) return audio;
    Object shotgun = LoadObject(kShotgunSfx);
    if (!base || !shotgun) return base.ptr;
    Object a = NewObject(base.Class(), Object(API->WorldContext()));
    if (!a) return base.ptr;
    for (GFProperty* p = API->NextProperty(base.Struct(), nullptr); p; p = API->NextProperty(base.Struct(), p)) {
        GML_PropInfo i;
        if (!API->GetPropertyInfo(p, &i) || !i.type || strcmp(i.type, "ObjectProperty")) continue;
        std::memcpy((uint8_t*)a.ptr + i.offset, (uint8_t*)base.ptr + i.offset, sizeof(void*));
    }
    for (const char* s : {"ShotSound", "TailSound"})
        if (GUObject* snd = shotgun.Get<GUObject*>(s)) a.Set<GUObject*>(s, snd);
    API->KeepAlive(a.ptr);
    audio = a.ptr;
    Log("Saiga sound set: {} with the Benelli's shot ({})", base.Name(), Object(a.Get<GUObject*>("ShotSound")).Name());
    return audio;
}

// The charging handle's own copy of the receiver's ChargingHandleSettings (unreflected, past
// GrippableBoxComponent's properties; in this build at +0x65C): its fields from Axis to HapticAmplitude, which
// is what it copies. Its flags don't match byte for byte, so only the axis and the five numbers are compared
// (travel, return speed, the safety's stop, haptics). The handle travels by it.
static uint8_t* HandleSettingsCopy(Object handle, const uint8_t* settings, GUStruct* st) {
    if (!handle || !st) return nullptr;
    int from = API->StructSize((GUStruct*)FindClass("GrippableBoxComponent")), to = API->StructSize(handle.Struct()), n = 0;
    std::vector<GML_PropInfo> all;
    for (GFProperty* p = API->NextProperty(st, nullptr); p; p = API->NextProperty(st, p)) {  // not in offset order
        GML_PropInfo i{};
        char name[64] = "";
        if (!API->GetPropertyInfo(p, &i)) continue;
        all.push_back(i);
        API->GetPropertyName(p, name, sizeof name);
        if (!strcmp(name, "HapticAmplitude")) n = i.offset + i.size;
    }
    std::vector<GML_PropInfo> fields;
    for (auto& i : all)
        if (i.offset < n && strcmp(i.type, "BoolProperty")) fields.push_back(i);
    if (from <= 0 || n <= 0 || fields.size() < 4) return nullptr;
    for (int o = (from + 3) & ~3; o + n <= to; o += 4) {
        auto* c = (uint8_t*)handle.ptr + o;
        bool same = std::all_of(fields.begin(), fields.end(), [&](const GML_PropInfo& f) {
            return !memcmp(c + f.offset, settings + f.offset, (size_t)f.size * (std::max)(1, f.arrayDim));
        });
        if (same) return c;
    }
    return nullptr;
}

// The receiver as a Saiga-12K: 12 gauge rounds and shells, the Benelli M4's rate of fire and recoil
// (scaled by [Gun] RecoilScale), its shot sound. Everything else (handling, selector, bolt) stays the AK's.
static void ConfigureGun(Object gun) {
    if (GUClass* round = RoundClass()) gun.Set<GUClass*>("BulletData", round);
    gun.Set("FireRateRPMMin", (float)s_fireRate);
    gun.Set("FireRateRPMMax", (float)s_fireRate);
    if (GUClass* benelli = LoadClass(kShotgunGun)) {
        Object ref = DefaultObject(benelli);
        for (const char* v : {"RotationPerShot", "LocationPerShot", "MaxRotation", "MaxLocation"}) {
            Vec3 x = ref.Get<Vec3>(v);
            if (!strncmp(v, "Max", 3)) gun.Set(v, x);  // limits stay the Benelli's
            else gun.Set(v, Vec3{x.x * s_recoilScale, x.y * s_recoilScale, x.z * s_recoilScale});
        }
    } else {
        Warn("{} not found - the Saiga keeps the AK-74M's recoil", kShotgunGun);
    }
    // The Saiga's carrier travels 12 cm: its tail stops a millimetre short of the recoil spring guide's rear
    // block, with the bolt face 1.3 cm behind the magazine (tools\prep's action study). The AK-74M's handle
    // travels 15 and would push it out the back. On safe the handle stops where it meets the Saiga's lever, 6.1
    // cm back (a real AK's stops about 6.3 cm back). The handle (the grab box your hand pulls) keeps a copy of
    // both, so hand, box and knob move together.
    if (GFProperty* hs = API->FindProperty(gun.Struct(), "ChargingHandleSettings")) {
        GML_PropInfo si{}, ti{}, li{};
        GFProperty* travel = API->GetPropertyInfo(hs, &si) && si.structType ? API->FindProperty(si.structType, "TravelDistance") : nullptr;
        GFProperty* safeStop = si.structType ? API->FindProperty(si.structType, "LockedOnSafeProgress") : nullptr;
        if (travel && API->GetPropertyInfo(travel, &ti) && safeStop && API->GetPropertyInfo(safeStop, &li)) {
            uint8_t* settings = (uint8_t*)gun.ptr + si.offset;
            float wasStop = *(float*)(settings + li.offset);
            Object handle = gun.GetObj("ChargingHandle");
            if (uint8_t* copy = HandleSettingsCopy(handle, settings, si.structType)) {
                *(float*)(copy + ti.offset) = kCarrierTravel;
                *(float*)(copy + li.offset) = kSafeStop;
                // a gun spawned on safe already holds its handle at the AK-74M's stop
                if (handle.GetBool("bIsLocked") && std::fabs(handle.Get<float>("HandleLockedProgress") - wasStop) < 1e-4f)
                    handle.Set("HandleLockedProgress", kSafeStop);
                Log("{}: the charging handle travels {:.0f} cm with the carrier and stops {:.1f} cm back on safe (its own copy at +0x{:X})",
                    gun.Name(), -kCarrierTravel, -kCarrierTravel * kSafeStop, copy - (uint8_t*)handle.ptr);
            } else
                Warn("{}: the charging handle's own settings were not found - it keeps the AK-74M's 15 cm", gun.Name());
            *(float*)(settings + ti.offset) = kCarrierTravel;
            *(float*)(settings + li.offset) = kSafeStop;
        }
    }
    gun.Set<GUObject*>("AudioData", SaigaAudio(gun.GetObj("AudioData")));
    Object shell = LoadObject(kShell), used = LoadObject(kShellUsed);
    if (shell) {
        gun.Set<GUObject*>("UnspentRoundMesh", shell.ptr);
        if (Object proxy = gun.GetObj("ProxyRound")) {
            Params sm(proxy, "SetStaticMesh");
            sm.SetObj("NewMesh", shell).Call();
        }
    }
    if (used) gun.Set<GUObject*>("SpentRoundMesh", used.ptr);
}

// A magazine for the Saiga: 5 rounds of 12 gauge (the receiver takes the round class from the magazine
// it seats).
static void LoadMagazine(Object mag) {
    if (!mag) return;
    if (GUClass* round = RoundClass(); round && mag.Get<GUClass*>("BulletData") != round) mag.Set<GUClass*>("BulletData", round);
    if (mag.Get<int32_t>("AmmoCapacity") != s_capacity) mag.Set<int32_t>("AmmoCapacity", s_capacity);
    if (mag.Get<int32_t>("CurrentAmmo") > s_capacity) mag.Set<int32_t>("CurrentAmmo", s_capacity);
}

// ------------------------------------------------------------------ slugs
//
// AGunReceiver::FireRoundLocal spawns its BulletData's round through AEBBullet::Spawn, which reads the
// class defaults: Shotgun + ShotCount make the 12 gauge round nine pellets, and a new round copies the
// defaults' mass, size, drag, penetration and damage curve. While a Saiga's FireRoundLocal runs, those
// defaults are a slug; pooling is off for that moment so the slug is a new actor with the slug's values
// (and is destroyed, not pooled, when it stops). Then the defaults go back, so every other gun's 12 gauge
// stays buckshot, and buckshot pellets in the pool stay pellets.

// Prologue of AGunReceiver::FireRoundLocal(const FVector&, const FVector&) in this build (RVA 0x5048110);
// "48 8B 99 A0 05 00 00" is its read of BulletData.
static const char* kFireRoundLocalSig =
    "40 55 53 56 57 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 00 "
    "48 8B 99 A0 05 00 00 49 8B F0 4C 8B FA 48 8B F9";
using FireRoundLocalFn = void (*)(GUObject* self, const void* direction, const void* origin);
static FireRoundLocalFn s_fireRoundLocal = nullptr;
static bool s_slugs = true;
static float s_slugDamage = 250, s_slugVelocity = 430, s_slugMass = 28;

static bool IsSaiga(GUObject* receiver);

// A linear speed (cm/s) -> damage curve for the slug, like the game's Dmg_* curves.
static GUObject* SlugDamageCurve() {
    static GUObject* curve = nullptr;
    if (curve && API->IsValid(curve)) return curve;
    Object c = NewObject(FindClass("CurveFloat"), Object(API->WorldContext()));
    GFProperty* fc = c ? API->FindProperty(c.Struct(), "FloatCurve") : nullptr;
    GML_PropInfo fi{}, ki{};
    if (!fc || !API->GetPropertyInfo(fc, &fi) || !fi.structType) return nullptr;
    GFProperty* kp = API->FindProperty(fi.structType, "Keys");
    if (!kp || !API->GetPropertyInfo(kp, &ki)) return nullptr;
    struct Key { uint8_t interp, tangentMode, tangentWeightMode, pad; float time, value, arrive, arriveWeight, leave, leaveWeight; };
    static_assert(sizeof(Key) == 0x1C);  // FRichCurveKey
    auto* keys = (GML_TArray*)((uint8_t*)c.ptr + fi.offset + ki.offset);
    auto* k = (Key*)API->ArrayAdd(keys, sizeof(Key), 2);
    if (!k) return nullptr;
    k[0] = {0, 0, 0, 0, 5000.f, 25.f};                                   // RCIM_Linear; 50 m/s
    k[1] = {0, 0, 0, 0, s_slugVelocity * 100.f, s_slugDamage};          // the muzzle
    API->KeepAlive(c.ptr);
    curve = c.ptr;
    Params check(c, "GetFloatValue");
    check.Set("InTime", s_slugVelocity * 100.f).Call();
    Params half(c, "GetFloatValue");
    half.Set("InTime", s_slugVelocity * 50.f).Call();
    Log("slug damage curve: {:.0f} at {:.0f} m/s, {:.0f} at {:.0f} m/s", check.Return<float>(), s_slugVelocity, half.Return<float>(),
        s_slugVelocity / 2);
    return curve;
}

struct SlugValue { const char* prop; float slug; float saved; };

static void FireRoundLocalDetour(GUObject* self, const void* direction, const void* origin) {
    if (!s_slugs || !IsSaiga(self)) { s_fireRoundLocal(self, direction, origin); return; }
    Object gun(self);
    GUClass* round = gun.Get<GUClass*>("BulletData");
    Object cdo = round ? DefaultObject(round) : Object();
    GUObject* curve = SlugDamageCurve();
    if (!cdo || !curve) { s_fireRoundLocal(self, direction, origin); return; }

    SlugValue values[] = {
        {"MuzzleVelocityMin", s_slugVelocity * 100.f}, {"MuzzleVelocityMax", s_slugVelocity * 100.f},
        {"Mass", s_slugMass / 1000.f},  // kg
        {"Diameter", 1.85f},            // cm: a 12 gauge Foster-type slug
        {"FormFactor", 1.6f},           // a blunt slug against the G1 drag curve
        {"MinPenetration", 4.5f}, {"MaxPenetration", 5.0f},
    };
    for (auto& v : values) { v.saved = cdo.Get<float>(v.prop); cdo.Set<float>(v.prop, v.slug); }
    bool shotgun = cdo.GetBool("Shotgun"), pooling = cdo.GetBool("EnablePooling");
    GUObject* damage = cdo.Get<GUObject*>("DamageCurve");
    cdo.SetBool("Shotgun", false);
    cdo.SetBool("EnablePooling", false);
    cdo.Set<GUObject*>("DamageCurve", curve);

    s_fireRoundLocal(self, direction, origin);

    for (auto& v : values) cdo.Set<float>(v.prop, v.saved);
    cdo.SetBool("Shotgun", shotgun);
    cdo.SetBool("EnablePooling", pooling);
    cdo.Set<GUObject*>("DamageCurve", damage);
}

// The same function once another plugin hooked it (the Barrett M107A1 does too): GML put a 5-byte jmp in place of
// its first five bytes, to a relay that jumps on to that plugin's detour.
static const char* kFireRoundLocalHookedSig =
    "E9 ?? ?? ?? ?? 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 00 "
    "48 8B 99 A0 05 00 00 49 8B F0 4C 8B FA 48 8B F9";
static void* FollowJumps(void* p) {  // jmp rel32 / jmp [rip+0] chains, to the code they end at
    for (int i = 0; i < 16; i++) {
        auto* b = (uint8_t*)p;
        if (b[0] == 0xE9) p = b + 5 + *(int32_t*)(b + 1);
        else if (b[0] == 0xFF && b[1] == 0x25) p = *(void**)(b + 6 + *(int32_t*)(b + 2));
        else break;
    }
    return p;
}

static void InstallSlugs() {
    if (!s_slugs) { Log("slugs off: the Saiga fires the game's buckshot"); return; }
    void* target = API->FindPattern(kFireRoundLocalSig);
    if (!target) {
        // Hooked by another plugin first: hook the end of its jumps (its detour), so each plugin's detour runs in
        // turn and passes on the guns that aren't its own.
        if (void* hooked = API->FindPattern(kFireRoundLocalHookedSig)) {
            target = FollowJumps(hooked);
            Log("slugs: FireRoundLocal (exe+0x{:X}) is already hooked by another plugin - chaining after it",
                (uintptr_t)hooked - (uintptr_t)API->ImageBase());
        }
    }
    if (!target) {
        Warn("AGunReceiver::FireRoundLocal not found (game updated?) - the Saiga fires buckshot");
        return;
    }
    if (!API->HookNative(target, (void*)&FireRoundLocalDetour, (void**)&s_fireRoundLocal) || !s_fireRoundLocal) {
        Error("could not hook FireRoundLocal - the Saiga fires buckshot");
        s_fireRoundLocal = nullptr;
        return;
    }
    Log("slugs: FireRoundLocal hooked at exe+0x{:X}", (uintptr_t)target - (uintptr_t)API->ImageBase());
}

// ------------------------------------------------------------------ magazines
//
// The AK-74M takes the game's 6L23 magazine. A magazine that goes into a Saiga - and the spare ones you
// carry while you have one - becomes the Sb.5: 5 rounds of 12 gauge, and the Sb.5's mesh in place of the
// 6L23's. The Sb.5 geometry is in gun space as if seated, so it hangs on the magazine at the inverse of
// where a seated magazine sits in the gun (measured once from the first seated magazine).

static std::vector<GUObject*> s_dressedMags;
static Xf s_magInGun;  // magazine actor transform relative to the gun, fully inserted

// Only this plugin gives an AK-74M receiver or its magazine DynamicMeshComponents (the Saiga's meshes).
static bool IsDressed(Object actor) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("DynamicMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    return arr && arr->Num > 0;
}

// The game draws the rounds (instanced meshes) for its magazine; the Sb.5 is solid, so they stay hidden.
static void HideRounds(Object mag) {
    Params gc(mag, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("InstancedStaticMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (c.GetBool("bVisible")) HideComponent(c);
    }
}

static void FitMagGrip(Object mag);

static void DressMagazine(Object mag, Object insertedIn) {
    if (!mag) return;
    LoadMagazine(mag);
    if (!s_ownMagazines || !s_magMesh || IsDressed(mag)) return;
    if (insertedIn && s_magInGun.empty()) {
        // The reference comes from a magazine seated in the gun (its CurrentMagazine) that sits in the
        // same place on two checks a quarter second apart - not one still sliding in or out.
        static Xf candidate;
        if (insertedIn.GetObj("CurrentMagazine").ptr != mag.ptr) return;
        Xf now = RelativeTo(WorldXf(mag), WorldXf(insertedIn));
        bool steady = false;
        if (!candidate.empty()) {
            Params eq(Lib("KismetMathLibrary"), "NearlyEqual_TransformTransform");
            PutXf(eq, "A", candidate);
            PutXf(eq, "B", now);
            eq.Set("LocationTolerance", 0.05f).Set("RotationTolerance", 0.2f).Set("Scale3DTolerance", 0.001f).Call();
            steady = eq.Return<bool>();
        }
        candidate = now;
        if (!steady) return;
        s_magInGun = now;
        Log("magazine reference taken from {} seated in {}", mag.Name(), insertedIn.Name());
    }
    if (s_magInGun.empty()) return;  // no reference yet: dressed once one has been seen seated
    Object root = mag.GetObj("RootComponent");
    Object comp = API->AddDynamicMeshComponent(mag.ptr, s_magMesh, s_mids, kNumParts);
    if (!root || !comp) return;
    SetRelativeXf(comp, Invert(s_magInGun));
    HideComponent(root);
    HideRounds(mag);
    FitMagGrip(mag);
    s_dressedMags.push_back(mag.ptr);
    Log("magazine {} is now a Saiga Sb.5 ({} rounds)", mag.Name(), mag.Get<int32_t>("CurrentAmmo"));
}

// ------------------------------------------------------------------ dressing

struct DressedGun { GUObject* gun; GUObject* muzzle; };
static std::vector<DressedGun> s_dressedGuns;  // also what the slug hook checks

static bool IsSaiga(GUObject* receiver) {
    for (auto& d : s_dressedGuns)
        if (d.gun == receiver) return API->IsValid(receiver);
    return false;
}

// ------------------------------------------------------------------ the Saiga's own action
//
// The Saiga's moving parts are its own meshes, posed every frame by this plugin about the Saiga's own pivots
// (the Tarkov rig's node origins, fitted by tools\prep), from the state the game keeps on the receiver:
//   bolt carrier + charging handle  slides as far as the game moves the AK-74M's carrier bone (firing cycles,
//                                   a pulled charging handle, the safety's stop); 12 cm at most, the room the
//                                   Saiga's receiver has
//   bolt                            stays put for the carrier's first 0.55 cm, turns 35 degrees to unlock over
//                                   the next 0.35, then travels with it (and the reverse going home)
//   fire selector                   the Saiga's lever on its own axle: SAFE all the way up (-20 degrees), its
//                                   top edge closing the slot under the dust cover and its front end in the
//                                   charging handle's path, as on an AK; AUTO -8, the highest it clears the
//                                   handle's full stroke; SEMI down at the "F" (0, where Tarkov models it). The
//                                   AK-74M's angles (11.5 / 20.5 down) put the lever across the Saiga's magazine.
//                                   It swings between them at 240 degrees a second.
//   trigger                         back 10 degrees on its pin with the trigger pull (CurrentTrigger)
//   magazine release                its paddle swings forward 12 degrees while the release is pressed
// The AK-74M's movables stay hidden; only its carrier bone is read. The charging handle's grab box moves from
// the AK-74M's knob onto the Saiga's.

struct Q { double x, y, z, w; };
static Q AxisAngle(int axis, double deg) {
    double h = deg * 3.14159265358979 / 360;
    Q q{0, 0, 0, std::cos(h)};
    (&q.x)[axis] = std::sin(h);
    return q;
}
static Vec3 Rotate(const Q& q, Vec3 v) {  // FQuat::RotateVector
    Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

// An FTransform as the engine lays it out (doubles): rotation quaternion, translation, scale.
struct XfD { double q[4]; double t[3]; double pad0; double s[3]; double pad1; };
static_assert(sizeof(XfD) == 0x60);

static bool XfLayoutOk() {
    static int ok = -1;
    if (ok < 0) {
        Params mk(Lib("KismetMathLibrary"), "MakeTransform");
        struct { double pitch, yaw, roll; } yaw90{0, 90, 0};
        mk.Set("Location", Vec3{1, 2, 3}).Set("Rotation", yaw90).Set("Scale", Vec3{1, 1, 1}).Call();
        XfD x{};
        ok = XfSize() == sizeof x;
        if (ok) {
            std::memcpy(&x, mk.Ptr("ReturnValue"), sizeof x);
            ok = x.t[0] == 1 && x.t[1] == 2 && x.t[2] == 3 && x.s[0] == 1 && std::fabs(std::fabs(x.q[2]) - 0.7071068) < 1e-5 &&
                 std::fabs(x.q[3]) > 0.7;
        }
        if (!ok) Warn("FTransform layout is not the expected one - the Saiga's parts ride the AK-74M's bones instead");
    }
    return ok == 1;
}

// The magazine's grip (its hand socket, the hand you take it by) is made for the AK-74M's slim, curved
// magazine: a fist the magazine passes through. The game puts your hand on its own IK target from the socket
// (with the player's ProxyHandSocketOffset), and on the Sb.5 that fist's palm and fingers sink 2 cm in; the
// Sb.5 is too deep to pass through a fist. On an Sb.5 the grip is turned and moved so the hand closes around it:
// fingers at 80% of their curl wrapped round its front, the hand rolled 22 degrees about its palm (so the magazine
// sits in your hand close to how the AK-74M's does), the palm, thumb and middle to little fingers 2.5 to 4 mm off it
// and the thumb tip 1.5 cm under its top. The right hand takes the mirror image and fits the same, within 0.5 mm.
// tests\SaigaDev gripped a spare Sb.5 with the player's own hands (held where the arm reaches its IK target, as
// in VR) at several curls and wrote the hands out; gripfit searched turns, moves and curls for them.
static const Vec3 kMagGripMove{-3.735, 3.000, -2.267};       // on the magazine's root, cm: the hand off the Sb.5 first,
static const Q kMagGripTurn{0.19080, -0.00857, 0.00167, 0.98159};  // then turned about its palm,
static const Vec3 kMagGripPivot{-9.6939, 0.653, -14.2131};
static const Vec3 kMagGripShift{4.940, -4.150, 4.750};      // and moved onto it
static constexpr double kMagGripCurl = 0.8;
static void FitMagGrip(Object mag) {
    Object sock = mag.GetObj("HandSocket");
    if (!sock || !XfLayoutOk()) return;
    Vec3 l = sock.Get<Vec3>("RelativeLocation");
    l = {l.x + kMagGripMove.x - kMagGripPivot.x, l.y + kMagGripMove.y - kMagGripPivot.y, l.z + kMagGripMove.z - kMagGripPivot.z};
    Vec3 r = Rotate(kMagGripTurn, l);
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Location", Vec3{r.x + kMagGripPivot.x + kMagGripShift.x, r.y + kMagGripPivot.y + kMagGripShift.y, r.z + kMagGripPivot.z + kMagGripShift.z})
        .Set("Rotation", sock.Get<Vec3>("RelativeRotation")).Set("Scale", sock.Get<Vec3>("RelativeScale3D")).Call();
    XfD x;
    std::memcpy(&x, mk.Ptr("ReturnValue"), sizeof x);
    const Q& a = kMagGripTurn;  // turn after the socket's own rotation: a * q
    double q0[4] = {x.q[0], x.q[1], x.q[2], x.q[3]};
    x.q[3] = a.w * q0[3] - a.x * q0[0] - a.y * q0[1] - a.z * q0[2];
    x.q[0] = a.w * q0[0] + a.x * q0[3] + a.y * q0[2] - a.z * q0[1];
    x.q[1] = a.w * q0[1] - a.x * q0[2] + a.y * q0[3] + a.z * q0[0];
    x.q[2] = a.w * q0[2] + a.x * q0[1] - a.y * q0[0] + a.z * q0[3];
    Params st(sock, "K2_SetRelativeTransform");
    std::memcpy(st.Ptr("NewTransform"), &x, sizeof x);
    st.Set("bSweep", false).Set("bTeleport", true).Call();
    // the fingers: each joint's turn from the open hand, kMagGripCurl of it (FBPVRHandPoseBonePair: name, pad, FQuat)
    struct Pair { uint64_t bone, pad; double q[4]; uint8_t rest[0x10]; };
    GML_PropInfo ai{}, ei{};
    GFProperty* deltas = API->FindProperty(sock.Struct(), "CustomPoseDeltas");
    if (!deltas || !API->GetPropertyInfo(deltas, &ai) || !ai.inner || !API->GetPropertyInfo(ai.inner, &ei) || ei.size != (int)sizeof(Pair)) {
        Warn("{}: the magazine's grip has no finger poses in the expected layout - its fingers keep their curl", mag.Name());
        return;
    }
    if (auto* d = (GML_TArray*)sock.PropPtr("CustomPoseDeltas"))
        for (int i = 0; i < d->Num; i++) {
            double* q = ((Pair*)d->Data)[i].q;
            double sgn = q[3] < 0 ? -1 : 1, half = std::acos(std::clamp(std::fabs(q[3]), 0.0, 1.0)), s = std::sin(half);
            if (s < 1e-9) continue;
            double k = std::sin(half * kMagGripCurl) / s;
            for (int j = 0; j < 3; j++) q[j] *= sgn * k;
            q[3] = std::cos(half * kMagGripCurl);
        }
}

// x -> q (x - pivot) + pivot + move, as the part's relative transform on the gun's root (actor space).
static void PosePart(GUObject* part, Q q, Vec3 pivot, Vec3 move) {
    Vec3 r = Rotate(q, pivot);
    XfD x{{q.x, q.y, q.z, q.w}, {pivot.x - r.x + move.x, pivot.y - r.y + move.y, pivot.z - r.z + move.z}, 0, {1, 1, 1}, 0};
    Params p(Object(part), "K2_SetRelativeTransform");
    std::memcpy(p.Ptr("NewTransform"), &x, sizeof x);
    p.Set("bSweep", false).Set("bTeleport", true).Call();
}

// Actor space, cm (tools\prep, fitted), before the [Visuals] offset nudge.
static const Vec3 kSelectorPivot{-8.691, 1.972, -0.992}, kTriggerPivot{-6.474, -0.001, -3.508}, kReleasePivot{-0.349, 0.004, -3.640};
static const Vec3 kBoreAxis{0, 0.005, -0.304};  // the bolt turns about the bore line
static constexpr double kSelectorSafe = -20, kSelectorAuto = -8, kSelectorSemi = 0, kSelectorSpeed = 240;
static constexpr double kTriggerPull = 10, kReleasePress = -12, kReleaseSpeed = 400;
static constexpr double kBoltFree = 0.55, kBoltTurn = 0.35, kBoltUnlock = 35;

// The charging handle's grab box: the receiver's ChargingHandle, a GunHandleComponent (3 x 6 x 3 cm) with the
// hand poses for racking it attached under it. It moves by as much as the Saiga's knob sits from the AK-74M's:
// the centres of the outer centimetre of each knob, (8.504, 4.224, 1.142) on AK74M_movables and (8.389, 4.052,
// 0.885) on tools\prep's Saiga carrier, 3.3 mm apart.
static const Vec3 kAkHandleBox{8.571, 4.319, 1.213};  // where the AK-74M's blueprint has it
static const Vec3 kHandleBox{8.457, 4.146, 0.956};    // the same spot on the Saiga's knob

struct Rig {
    GUObject* gun;
    GUObject* mov;
    GUObject* part[kNumMoving];
    Vec3 carrierRest;
    double selector, release;  // animated angles, degrees
    double applied[kNumMoving][2];
    ULONGLONG tick;
    GUObject* handle = nullptr;  // the grab box, moved onto the Saiga's knob
    Vec3 handleShift{};
    int handleFixes = 0;  // times the game put it back on the AK-74M's knob
};
static std::vector<Rig> s_rigs;

static Vec3 Nudged(Vec3 p) { return {p.x + s_offset[0], p.y + s_offset[1], p.z + s_offset[2]}; }

static double SelectorTarget(uint8_t fireMode) {  // EGunFireMode
    return fireMode == 0 ? kSelectorSafe : fireMode == 1 ? kSelectorSemi : kSelectorAuto;
}

static Vec3 HandleShift() {
    Vec3 to = Nudged(kHandleBox);
    return {to.x - kAkHandleBox.x, to.y - kAkHandleBox.y, to.z - kAkHandleBox.z};
}

static void SetRelativeLocation(GUObject* c, Vec3 v) {
    Params p(Object(c), "K2_SetRelativeLocation");
    p.Set("NewLocation", v).Set("bSweep", false).Set("bTeleport", true).Call();
}

// Moves the grab box onto the Saiga's knob, with any copy the component keeps of where it sits (unreflected,
// past GrippableBoxComponent's properties: where its travel starts).
static GUObject* FitHandle(Object actor) {
    Object h = actor.GetObj("ChargingHandle");
    if (!h) { Warn("{} has no ChargingHandle - the grab point stays the AK-74M's", actor.Name()); return nullptr; }
    Vec3 d = HandleShift(), now = h.Get<Vec3>("RelativeLocation");
    int from = API->StructSize((GUStruct*)FindClass("GrippableBoxComponent")), to = API->StructSize(h.Struct());
    int copies = 0;
    for (int o = (from + 7) & ~7; from > 0 && o + 24 <= to; o += 8) {
        auto* v = (double*)((uint8_t*)h.ptr + o);
        auto at = [&](Vec3 p, double tol) { return std::fabs(v[0] - p.x) < tol && std::fabs(v[1] - p.y) < tol && std::fabs(v[2] - p.z) < tol; };
        if (!at(kAkHandleBox, 1e-3) && !at(now, 1e-6)) continue;
        v[0] += d.x; v[1] += d.y; v[2] += d.z;
        Log("{}: the charging handle keeps its spot at +0x{:X} too - moved", actor.Name(), o);
        copies++;
        o += 16;
    }
    Vec3 want{now.x + d.x, now.y + d.y, now.z + d.z};
    SetRelativeLocation(h.ptr, want);
    Vec3 got = h.Get<Vec3>("RelativeLocation");
    if (std::fabs(got.x - want.x) + std::fabs(got.y - want.y) + std::fabs(got.z - want.z) > 0.001)
        Error("{}: the charging handle grab point did not move (at {:.3f},{:.3f},{:.3f})", actor.Name(), got.x, got.y, got.z);
    Log("{}: charging handle grab point moved {:.1f} mm onto the Saiga's knob ({} stored cop{})", actor.Name(),
        10 * std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), copies, copies == 1 ? "y" : "ies");
    return h.ptr;
}

// If the game puts the grab box back on the AK-74M's knob (on the line its travel runs along), it goes back
// onto the Saiga's, wherever along the travel it is.
static void KeepHandle(Rig& r) {
    if (!r.handle || !API->IsValid(r.handle)) return;
    Vec3 d = r.handleShift, at = Object(r.handle).Get<Vec3>("RelativeLocation");
    if (std::fabs(d.y) + std::fabs(d.z) < 0.01 || std::fabs(at.y - kAkHandleBox.y) > 0.01 || std::fabs(at.z - kAkHandleBox.z) > 0.01) return;
    SetRelativeLocation(r.handle, {at.x + d.x, at.y + d.y, at.z + d.z});
    if (r.handleFixes++ % 1000 == 0)
        Log("{}: the game put the charging handle grab point back on the AK-74M's knob (x{}) - moved onto the Saiga's again",
            Object(r.gun).Name(), r.handleFixes);
}

static void PoseRig(Rig& r) {
    Object gun(r.gun);
    ULONGLONG now = GetTickCount64();
    double dt = (std::min)(0.1, (now - r.tick) / 1000.0);
    r.tick = now;

    // the carrier: where the game has the AK-74M's carrier bone now, against its rest
    Vec3 carrier{0, 0, 0};
    {
        static uint64_t bone = API->MakeName("RootBoltCarrier");
        Params bt(Object(r.mov), "GetBoneTransformByName");
        *(uint64_t*)bt.Ptr("BoneName") = bone;
        bt.Set("BoneSpace", (uint8_t)1).Call();  // component space (the movables sit at the actor's origin)
        XfD x{};
        std::memcpy(&x, bt.Ptr("ReturnValue"), sizeof x);
        carrier = {x.t[0] - r.carrierRest.x, x.t[1] - r.carrierRest.y, x.t[2] - r.carrierRest.z};
    }
    double travel = (std::max)(0.0, -carrier.x);
    auto step = [&](double& angle, double target, double speed) {
        double d = target - angle, s = speed * dt;
        angle = std::fabs(d) <= s ? target : angle + (d > 0 ? s : -s);
    };
    step(r.selector, SelectorTarget(gun.Get<uint8_t>("FireMode")), kSelectorSpeed);
    step(r.release, gun.GetBool("bIsMagReleasePressed") ? kReleasePress : 0, kReleaseSpeed);
    double trigger = std::clamp((double)gun.Get<float>("CurrentTrigger"), 0.0, 1.0) * kTriggerPull;
    double turn = std::clamp((travel - kBoltFree) / kBoltTurn, 0.0, 1.0) * kBoltUnlock;

    for (int i = 0; i < kNumMoving; i++) {
        if (!r.part[i]) continue;
        double a = 0, b = 0;
        Q q{0, 0, 0, 1};
        Vec3 pivot{0, 0, 0}, move{0, 0, 0};
        switch (s_movingParts[i].kind) {
            case PartKind::Carrier: a = carrier.x; b = carrier.z; move = carrier; break;
            case PartKind::BoltHead:
                a = travel; b = turn;
                q = AxisAngle(0, turn);  // right side up: counterclockwise seen from behind
                pivot = Nudged(kBoreAxis);
                move = {-(std::max)(0.0, travel - kBoltFree - kBoltTurn), 0, 0};
                break;
            case PartKind::Selector: a = r.selector; q = AxisAngle(1, r.selector); pivot = Nudged(kSelectorPivot); break;
            case PartKind::Trigger: a = trigger; q = AxisAngle(1, trigger); pivot = Nudged(kTriggerPivot); break;
            case PartKind::MagRelease: a = r.release; q = AxisAngle(1, r.release); pivot = Nudged(kReleasePivot); break;
        }
        if (std::fabs(a - r.applied[i][0]) < 1e-4 && std::fabs(b - r.applied[i][1]) < 1e-4) continue;
        r.applied[i][0] = a;
        r.applied[i][1] = b;
        PosePart(r.part[i], q, pivot, move);
    }
    KeepHandle(r);
}

static void PoseRigs() {  // every frame
    for (size_t i = 0; i < s_rigs.size();) {
        Rig& r = s_rigs[i];
        if (!API->IsValid(r.gun) || !API->IsValid(r.mov)) { s_rigs.erase(s_rigs.begin() + i); continue; }
        PoseRig(r);
        i++;
    }
}

// Fallback: each part rides the AK-74M bone the game drives (the AK-74M's pivots and angles).
static int RideBones(Object actor, Object mov, Xf movInActor) {
    int hung = 0;
    for (auto& p : s_movingParts) {
        if (!p.mesh) continue;
        Xf rest = RefPoseComponent(mov, p.bone);
        if (rest.empty()) continue;
        Object comp = API->AddDynamicMeshComponent(actor.ptr, p.mesh, s_mids, kNumParts);
        if (!comp) continue;
        AttachTo(comp, mov, p.bone);
        SetRelativeXf(comp, Invert(Compose(rest, movInActor)));  // actor-space vertices on the bone
        hung++;
    }
    return hung;
}

static void HangMovingParts(Object actor) {
    Object mov = actor.GetObj("Movables");  // AK-74M moving parts: a PoseableMesh the gun code poses by bone name
    if (!mov) { Warn("{} has no Movables component - keeping the AK-74M's moving parts", actor.Name()); return; }
    Xf movInActor = RelativeTo(WorldXf(mov), WorldXf(actor));
    Xf carrierRest = RefPoseComponent(mov, "RootBoltCarrier");
    int hung = 0;
    if (XfLayoutOk() && !carrierRest.empty()) {
        Rig r{actor.ptr, mov.ptr, {}, {}, kSelectorSemi, 0, {}, GetTickCount64()};
        XfD rest{};
        std::memcpy(&rest, carrierRest.data(), sizeof rest);
        r.carrierRest = {rest.t[0], rest.t[1], rest.t[2]};
        r.selector = SelectorTarget(actor.Get<uint8_t>("FireMode"));
        for (int i = 0; i < kNumMoving; i++) {
            r.part[i] = s_movingParts[i].mesh ? API->AddDynamicMeshComponent(actor.ptr, s_movingParts[i].mesh, s_mids, kNumParts) : nullptr;
            for (auto& v : r.applied[i]) v = 1e9;  // posed on the first frame
            hung += r.part[i] != nullptr;
        }
        r.handle = FitHandle(actor);
        r.handleShift = HandleShift();
        s_rigs.push_back(r);
        PoseRig(s_rigs.back());
    } else {
        hung = RideBones(actor, mov, movInActor);
        FitHandle(actor);
    }
    // Stop drawing the AK-74M's parts but keep the bones posed: the carrier's is read every frame.
    mov.Set("VisibilityBasedAnimTickOption", (uint8_t)0);  // AlwaysTickPoseAndRefreshBones
    HideComponent(mov);
    Log("{}: {} Saiga moving parts{}", actor.Name(), hung, s_rigs.empty() || s_rigs.back().gun != actor.ptr ? " on the AK-74M's bones" : ", posed by the plugin");
}

static void Mark(Object receiver);

static void DressAsSaiga(Object actor) {
    if (IsDressed(actor)) return;
    if (!BuildVisualAssets()) return;
    Object root = actor.GetObj("StaticMeshComponent");  // the AK-74M mesh; also collision + physics
    if (!root) { Error("{} has no StaticMeshComponent", actor.FullName()); return; }
    Object comp = API->AddDynamicMeshComponent(actor.ptr, s_mesh, s_mids, kNumParts);
    if (!comp) { Error("could not add the Saiga mesh to {}", actor.FullName()); return; }
    Object muzzle = s_muzzleMesh ? Object(API->AddDynamicMeshComponent(actor.ptr, s_muzzleMesh, s_mids, kNumParts)) : Object();
    HideComponent(root);
    if (s_ownMovingParts) HangMovingParts(actor);
    if (s_fitAttachments) FitAttachPoints(actor);
    ConfigureGun(actor);
    s_dressedGuns.push_back({actor.ptr, muzzle.ptr});
    DressMagazine(actor.GetObj("CurrentMagazine"), actor);
    Log("*** {} dressed as a Saiga-12K: 12 gauge {}, {} rounds, {} rpm ***", actor.Name(), s_slugs && s_fireRoundLocal ? "slugs" : "buckshot",
        s_capacity, s_fireRate);
}

// Topmost actor this one is attached to (a magazine in a pouch on a player -> the player).
static GUObject* AttachRoot(Object a) {
    for (int guard = 0; guard < 16 && a; guard++) {
        Params p(a, "GetAttachParentActor");
        p.Call();
        Object up = p.Return<GUObject*>();
        if (!up) break;
        a = up;
    }
    return a.ptr;
}

// ------------------------------------------------------------------ multiplayer
//
// Each machine dresses guns itself, and only the one whose player picked the Saiga (or loaded it from a
// save slot) knows the AK-74M it got is one. It tells the others through the receiver's own
// ColorComponent: its ColorOption is an FName the game replicates to every player and keeps in a saved
// loadout. The plugin sets it to kMarker with the game's own colour RPC, and any machine with the plugin
// that sees an AK-74M receiver with that colour dresses it. A machine without the plugin ignores the
// unknown colour and shows an AK-74M (firing buckshot). Untested in a real multiplayer session.

static constexpr char kMarker[] = "Saiga12K";
static const char* kSelectHook =
    "WB_GunCustomization_C:BndEvt__WB_GunCustomization_WB_Database_K2Node_ComponentBoundEvent_34_OnItemSelected__DelegateSignature";
static std::map<GUObject*, GUObject*> s_picks;  // gun station -> gun last picked at it on this machine

static bool HasAuthority(Object actor) {
    Params p(actor, "HasAuthority");
    p.Call();
    return p.Return<bool>();
}

// A slug that arrives on another machine. In multiplayer the host spawns every round (a client's shot reaches it as
// FireRoundServer, and the slug hook makes it a slug there, so its damage is right), and the other machines get a
// copy of it with only the replicated values: there it would be a buckshot pellet with a pellet's mass, size and
// penetration, and fly and strike like one. A round of the Saiga's round class fired by the player who owns a
// Saiga gets the slug's values.
static void DressArrivedRound(Object round) {
    for (auto& d : s_dressedGuns) {
        Object gun(d.gun);
        if (!API->IsValid(gun.ptr) || gun.Get<GUClass*>("BulletData") != round.Class()) continue;
        if (!s_slugs || HasAuthority(round)) return;
        Params gi(round, "GetInstigator");
        gi.Call();
        Params go(gun, "GetOwner");
        go.Call();
        Params hc(gun, "GetHoldingCharacter");
        hc.Call();
        GUObject* by = gi.Return<GUObject*>();
        GUObject* holder = hc.Return<GUObject*>();  // the gun in the shooter's hands (else, unknown here: theirs)
        if (!by || (holder ? holder != by : go.Return<GUObject*>() != by)) continue;
        round.Set<float>("Mass", s_slugMass / 1000.f);
        round.Set<float>("Diameter", 1.85f);
        round.Set<float>("FormFactor", 1.6f);
        round.Set<float>("MinPenetration", 4.5f);
        round.Set<float>("MaxPenetration", 5.0f);
        round.SetBool("Shotgun", false);
        if (GUObject* curve = SlugDamageCurve()) round.Set<GUObject*>("DamageCurve", curve);
        return;
    }
}

static std::string ColorOf(Object receiver) {
    Object cc = receiver ? receiver.GetObj("ColorComponent") : Object();
    char b[128] = "";
    if (cc) API->NameToString(cc.Get<uint64_t>("ColorOption"), b, sizeof b);
    return b;
}

static bool IsMarked(Object receiver) {
    return receiver.Class() == s_receiverClass && _stricmp(ColorOf(receiver).c_str(), kMarker) == 0;
}

// This machine's player owns the actor (it was spawned for them, or they hold it): the server takes
// RPCs on it from here.
static bool LocallyOwned(Object actor) {
    Object o = actor;
    for (int guard = 0; guard < 4 && o; guard++) {
        if (o.IsA("Pawn")) {
            Params lc(o, "IsLocallyControlled");
            lc.Call();
            return lc.Return<bool>();
        }
        if (o.IsA("PlayerController")) {
            Params lc(o, "IsLocalController");
            lc.Call();
            return lc.Return<bool>();
        }
        Params go(o, "GetOwner");
        go.Call();
        o = go.Return<GUObject*>();
    }
    return false;
}

// Names the gun a Saiga for every player: on the server the colour applies at once and replicates; from
// the client that owns the gun it goes to the server first. At most once a second per gun.
static void Mark(Object receiver) {
    static std::map<GUObject*, ULONGLONG> asked;
    Object cc = receiver.GetObj("ColorComponent");
    if (!cc || IsMarked(receiver)) return;
    bool authority = HasAuthority(receiver);
    if (!authority && !LocallyOwned(receiver)) return;  // someone else's gun: not ours to name
    ULONGLONG now = GetTickCount64();
    if (auto it = asked.find(receiver.ptr); it != asked.end() && now - it->second < 1000) return;
    asked[receiver.ptr] = now;
    Params ac(cc, "ApplyColor_Server");
    ac.Set("ColorName", API->MakeName(kMarker)).Call();
    Log("{}: named a Saiga for every player{}", receiver.Name(), authority ? "" : " (asked the server)");
}

// Every quarter second: the magazine in each Saiga, the local player's spare magazines while they have a
// Saiga, each Saiga's muzzle brake (hidden while a muzzle device replaces it), and the gun's settings
// (the game may reset its round class from a magazine or a loadout).
static void WatchDressedGuns() {
    static ULONGLONG next = 0;
    if (s_dressedGuns.empty() || GetTickCount64() < next) return;
    next = GetTickCount64() + 250;
    Object pawn;
    {
        Params p(Lib("GameplayStatics"), "GetPlayerPawn");
        p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
        pawn = p.Return<GUObject*>();
    }
    bool playerHasSaiga = false;
    GML_TArray* accepted = nullptr;
    for (size_t i = 0; i < s_dressedGuns.size();) {
        Object gun(s_dressedGuns[i].gun);
        if (!gun || !API->IsValid(gun.ptr) || !IsDressed(gun)) { s_dressedGuns.erase(s_dressedGuns.begin() + i); continue; }
        DressMagazine(gun.GetObj("CurrentMagazine"), gun);
        if (pawn && AttachRoot(gun) == pawn.ptr) playerHasSaiga = true;
        accepted = (GML_TArray*)gun.PropPtr("AcceptedMagazines");
        if (GUClass* round = RoundClass(); round && gun.Get<GUClass*>("BulletData") != round) gun.Set<GUClass*>("BulletData", round);
        Mark(gun);  // repainted at a station since, or the first try didn't reach the server: name it again
        if (Object muzzle = s_dressedGuns[i].muzzle; muzzle && API->IsValid(muzzle.ptr)) {
            bool device = false;
            Params ga(gun, "GetAttachedActors");
            ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
            auto* arr = (GML_TArray*)ga.Ptr("OutActors");
            for (int k = 0; arr && k < arr->Num && !device; k++) {
                Object att(((GUObject**)arr->Data)[k]);
                char tag[64] = "";
                if (att.IsA("GunAttachment")) API->NameToString(att.Get<uint64_t>("AttachPointTag"), tag, sizeof tag);
                device = !strcmp(tag, "MuzzleAttachLocation") && att.GetBool("bAttached");
            }
            Params v(muzzle, "SetVisibility");
            v.Set("bNewVisibility", !device).Set("bPropagateToChildren", false).Call();
        }
        i++;
    }
    for (size_t i = 0; i < s_dressedMags.size();) {  // keep their rounds hidden and their load 12 gauge
        if (!API->IsValid(s_dressedMags[i]) || !IsDressed(Object(s_dressedMags[i]))) { s_dressedMags.erase(s_dressedMags.begin() + i); continue; }
        HideRounds(s_dressedMags[i]);
        LoadMagazine(s_dressedMags[i]);
        i++;
    }
    if (playerHasSaiga && accepted && pawn) {
        for (int k = 0; k < accepted->Num; k++) {
            Object cls(((GUObject**)accepted->Data)[k]);
            for (auto& mag : FindAllOf(cls.Name().c_str()))
                if (AttachRoot(mag) == pawn.ptr) DressMagazine(mag, {});
        }
    }
}

// ------------------------------------------------------------------ loadouts (team room, operations)
//
// The game saves each carried gun as a GunSerializable in SaveGames\SavedGun_<slot>.sav, with its
// GunAsset stored as an object path, and rebuilds it from there at team-room start and in every operation.
// The Saiga's GunAsset is created at runtime, so its path only resolves in the session that wrote it.
// So once the game has saved the Saiga, the plugin re-saves that slot (with the game's own SaveGameToSlot)
// with the AK-74M donor as GunAsset - same receiver and attachments - and keeps the file's hash in its
// config. A loadout gun whose slot still holds exactly that file is the Saiga. Without the plugin the slot
// simply loads as an AK-74M.

static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}

static std::wstring Widen8(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static std::string ReadFileBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD n = 0;
    if (!ReadFile(f, s.data(), (DWORD)s.size(), &n, nullptr)) n = 0;
    CloseHandle(f);
    s.resize(n);
    return s;
}

static std::string HashHex(const std::string& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return std::format("{:016x}-{}", h, bytes.size());
}

static std::wstring SaveGamesDir() {
    static std::wstring dir;
    if (dir.empty()) {
        Params p(Lib("KismetSystemLibrary"), "GetProjectSavedDirectory");
        p.Call();
        char b[1024] = "";
        if (auto* s = (const GML_FString*)p.Ptr("ReturnValue")) API->FStringToUtf8(s, b, sizeof b);
        if (*b) dir = Widen8(b) + L"SaveGames\\";
    }
    return dir;
}

// [Loadouts] <slot> = <hash of the file while it holds the Saiga>, in the plugin's .cfg.
static ConfigEntry<std::string> SlotRecord(const std::string& slot) {
    return Config.Bind<std::string>("Loadouts", slot.c_str(), "", "Written by the plugin: this save slot holds the Saiga (hash of its file).");
}
static std::string RecordedHash(const std::string& slot) { return SlotRecord(slot).Value(); }
static void RecordHash(const std::string& slot, const std::string& hash) { SlotRecord(slot).Set(hash); }

// A field of FGunSetupStruct inside a loaded GunSerializable.
static void* SetupField(Object save, const char* name) {
    GFProperty* setupProp = save ? API->FindProperty(save.Struct(), "GunSetup") : nullptr;
    GML_PropInfo setup{}, field{};
    if (!setupProp || !API->GetPropertyInfo(setupProp, &setup) || !setup.structType) return nullptr;
    GFProperty* fieldProp = API->FindProperty(setup.structType, name);
    if (!fieldProp || !API->GetPropertyInfo(fieldProp, &field)) return nullptr;
    return (uint8_t*)save.ptr + setup.offset + field.offset;
}
static GUObject** GunAssetField(Object save) { return (GUObject**)SetupField(save, "GunAsset"); }

// The receiver colour a setup was saved with: SaveGunSetup keeps the receiver's ColorOption, which the plugin
// sets to kMarker on every Saiga (see "multiplayer").
static bool SavedWithMarker(Object save) {
    auto* name = (uint64_t*)SetupField(save, "SelectedColor");
    char b[128] = "";
    if (name) API->NameToString(*name, b, sizeof b);
    return _stricmp(b, kMarker) == 0;
}

struct SavedGun { GUObject* asset; bool marked; };

// The gun a gear manager saved in slot `slotProp`, read with the game's own GetGunSetup.
static SavedGun SavedGunSetup(Object gear, const char* slotProp) {
    std::string slot = FStr8(gear, slotProp);
    if (slot.empty()) return {};
    Params gg(gear, "GetGunSetup");
    gg.SetString("SlotName", Widen8(slot.c_str())).Call();
    Object save = gg.Get<GUObject*>("GunSetup");
    GUObject** f = GunAssetField(save);
    return {f ? *f : nullptr, SavedWithMarker(save)};
}

// SaveGunSetup names a gun by the first GunAsset in AvailableGuns with the receiver's class, so a Saiga is
// normally saved as the AK-74M donor, with the Saiga's colour name; one saved as our runtime asset (this
// session) or recorded by its hash (re-saved by the plugin) is the Saiga too.
static bool IsSaigaSlot(const std::string& slot, SavedGun saved) {
    if (saved.asset && saved.asset == s_gun) return true;  // written this session, not re-saved yet
    Object donor = FindObject(kDonor);
    if (!saved.asset || saved.asset != donor.ptr) return false;
    if (saved.marked) return true;
    std::string h = RecordedHash(slot);
    return !h.empty() && h == HashHex(ReadFileBytes(SaveGamesDir() + Widen8(slot.c_str()) + L".sav"));
}

// Re-save any gun slot the game wrote with the Saiga (or, from an earlier session, with a runtime GunAsset
// path that no longer resolves) so it names the donor; remember the new file's hash.
static void WatchLoadouts() {
    static ULONGLONG next = 0;
    static std::map<std::wstring, uint64_t> seen;  // file -> last write time
    if (!s_gun || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"SavedGun_*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        uint64_t stamp = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (seen[file] == stamp) continue;
        seen[file] = stamp;
        std::wstring stemW = file.substr(0, file.size() - 4);
        char slotBuf[260] = "";
        WideCharToMultiByte(CP_UTF8, 0, stemW.c_str(), -1, slotBuf, sizeof slotBuf, nullptr, nullptr);
        std::string slot = slotBuf;
        std::string raw = ReadFileBytes(dir + file);
        Params lg(Lib("GameplayStatics"), "LoadGameFromSlot");
        lg.SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
        Object save = lg.Return<GUObject*>();
        GUObject** asset = GunAssetField(save);
        if (!asset) continue;
        // A runtime GunAsset path in the file (saved while this plugin wasn't watching) is the Saiga only if the
        // slot held the Saiga before: other plugins (the Noveske) make runtime GunAssets too.
        size_t t = raw.find("/Engine/Transient.");
        bool staleRuntime = !*asset && t != std::string::npos && raw.find(".GunAsset_C_", t) != std::string::npos;
        bool staleSaiga = staleRuntime && !RecordedHash(slot).empty();
        if (*asset == s_gun || staleSaiga) {
            Object donor = FindObject(kDonor);
            if (!donor) continue;
            *asset = donor.ptr;
            Params sv(Lib("GameplayStatics"), "SaveGameToSlot");
            sv.SetObj("SaveGameObject", save).SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
            if (!sv.Return<bool>()) { Error("could not re-save loadout slot {}", slot); continue; }
            std::string hash = HashHex(ReadFileBytes(dir + file));
            RecordHash(slot, hash);
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + file).c_str(), GetFileExInfoStandard, &a))
                seen[file] = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            Log("loadout slot {} holds the Saiga: re-saved with a stable reference{}", slot, staleSaiga ? " (repaired a save from an earlier session)" : "");
        } else if (!RecordedHash(slot).empty() && !IsSaigaSlot(slot, {*asset, SavedWithMarker(save)})) {
            RecordHash(slot, "");  // the game saved a different gun there since
            Log("loadout slot {} no longer holds the Saiga", slot);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A receiver the gear manager rebuilt from your saved loadout (team room start, operations): it is ours
// if it became a player's PrimaryGun/SecondaryGun and that slot holds the Saiga.
static bool ResolveLoadout(Object actor) {
    for (auto& gear : FindAllOf("BP_GearManagerComponent_C")) {
        Object pawn(API->GetOuter(gear.ptr));
        if (pawn.IsA("Pawn")) {
            Params lc(pawn, "IsLocallyControlled");
            lc.Call();
            if (!lc.Return<bool>()) continue;
        }
        const char* slotProp = gear.GetObj("PrimaryGun").ptr == actor.ptr     ? "PrimaryGunSetupSlotName"
                               : gear.GetObj("SecondaryGun").ptr == actor.ptr ? "SecondaryGunSetupSlotName"
                                                                               : nullptr;
        if (!slotProp) continue;
        SavedGun saved = SavedGunSetup(gear, slotProp);
        std::string slot = FStr8(gear, "GunSetupSlotName") + "_" + FStr8(gear, slotProp);  // SavedGun_MainPrimary
        bool saiga = IsSaigaSlot(slot, saved);
        Log("{} loaded from {} ({}): saved gun '{}'{} -> {}", actor.Name(), slot, Object(API->GetOuter(gear.ptr)).Name(),
            saved.asset ? Object(saved.asset).Name() : std::string("?"), saved.marked ? " named a Saiga" : "", saiga ? "Saiga" : "not the Saiga");
        if (saiga) {
            DressAsSaiga(actor);
            Mark(actor);
        }
        return true;
    }
    return false;
}

// The spawner's menu items are WB_CustomizationItemGun widgets; the gun they stand for is their WeaponItem.
static GUObject* GunOf(GUObject* itemOrGun) {
    Object o(itemOrGun);
    if (o && o.IsA("WB_CustomizationItemGun_C")) return o.GetObj("WeaponItem").ptr;
    return itemOrGun;
}

static GUObject* s_lastPressedGun = nullptr;  // any spawner menu item pressed, most recent
static ULONGLONG s_lastPressedAt = 0;

// A gun station whose player is someone else's: this machine's menu presses say nothing about it.
static bool ForeignStation(Object station) {
    Object player = station.GetObj("Player");
    if (!player) return false;
    Params lc(player, "IsLocallyControlled");
    lc.Call();
    return !lc.Return<bool>();
}

// An AK-74M receiver began play. It is a Saiga if another machine named it one, if the gun station now
// holding it as CurrentReceiver was last asked for the Saiga entry on this machine, or if it is a loadout
// gun from a Saiga slot. The station sets CurrentReceiver right after the spawn returns, so this is checked
// on the following frames.
static void ResolvePending() {
    if (s_pending.empty()) return;
    auto stations = FindAllOf("CustomizationStation_Gun_C");
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        Object actor(p.actor);
        bool done = !actor || !API->IsValid(actor.ptr);
        if (!done && IsMarked(actor)) {
            Log("{} is named a Saiga by the game it came from", actor.Name());
            DressAsSaiga(actor);
            done = true;
        }
        for (auto& st : stations) {
            if (done || st.GetObj("CurrentReceiver").ptr != p.actor) continue;
            done = true;
            if (ForeignStation(st)) break;
            GUObject* picked = nullptr;
            const char* how = "?";
            if (auto pk = s_picks.find(st.ptr); pk != s_picks.end()) { picked = pk->second; how = "selection"; }
            if (!picked && GetTickCount64() - s_lastPressedAt < 30000) { picked = s_lastPressedGun; how = "last menu press"; }
            Log("{} spawned at {}: picked '{}' (via {}) -> {}", actor.Name(), st.Name(), picked ? Object(picked).Name() : std::string("?"),
                how, picked == s_gun ? "Saiga" : "stock AK-74M");
            if (picked == s_gun) {
                DressAsSaiga(actor);
                Mark(actor);
            }
        }
        if (!done) done = ResolveLoadout(actor);
        if (!done && GetTickCount64() - p.since > 5000) done = true;  // a world or enemy AK-74M stays one
        if (done) s_pending.erase(s_pending.begin() + i);
        else i++;
    }
}

// A level started: build the assets while it loads, and forget what was dressed (a new level's actors can
// reuse old addresses).
static void NewLevel() {
    BuildVisualAssets();
    SlugDamageCurve();
    std::erase_if(s_dressedGuns, [](const DressedGun& d) { return !API->IsValid(d.gun) || !IsDressed(Object(d.gun)); });
    std::erase_if(s_dressedMags, [](GUObject* m) { return !API->IsValid(m) || !IsDressed(Object(m)); });
    s_picks.clear();
}

// The level changed if the game state did (GML's WORLD_BEGIN_PLAY comes from the game mode, which only
// the host has; the game state is on every machine).
static void WatchLevel() {
    static ULONGLONG next = 0;
    static std::string level;
    if (GetTickCount64() < next) return;
    next = GetTickCount64() + 500;
    Params gs(Lib("GameplayStatics"), "GetGameState");
    gs.SetObj("WorldContextObject", Object(API->WorldContext())).Call();
    Object state = gs.Return<GUObject*>();
    if (!state) return;
    std::string now = state.FullName();
    if (now == level) return;
    level = now;
    NewLevel();
}

static void QueueReceiver(GUObject* actor) {
    for (auto& pe : s_pending)
        if (pe.actor == actor) return;
    s_pending.push_back({actor, GetTickCount64()});
}

static void InstallVisuals() {
    // The spawner's selection. On a client the station asks the server for the gun with the GunAsset it is
    // handed, and the server can't resolve this machine's runtime Saiga asset: for that call the menu item
    // stands for the AK-74M donor (same receiver), and this machine remembers the Saiga was picked.
    static Object swapped;
    Hook(kSelectHook,
         [](Object widget, GUFunction* fn, void* params) {
             Object item(Param<GUObject*>(fn, params, "Item"));
             if (!item || !item.IsA("WB_CustomizationItemGun_C")) return false;
             GUObject* gun = item.GetObj("WeaponItem").ptr;
             Object station = widget.GetObj("OwningStation");
             if (station) s_picks[station.ptr] = gun;
             if (gun != s_gun || !station || HasAuthority(station)) return false;
             Object donor = FindObject(kDonor);
             if (!donor) return false;
             item.Set<GUObject*>("WeaponItem", donor.ptr);
             swapped = item;
             Log("{}: asking the server for the AK-74M donor; this machine dresses it as the Saiga", station.Name());
             return false;
         },
         [](Object widget, GUFunction* fn, void* params) {
             if (swapped) {
                 swapped.Set<GUObject*>("WeaponItem", s_gun);
                 swapped = {};
             }
             Log("spawner selection in {}: {}", widget.Name(), Object(GunOf(Param<GUObject*>(fn, params, "Item"))).Name());
         });
    HookAfter("WB_CustomizationItemGun_C:BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature",
              [](Object item, GUFunction*, void*) {
                  if (GUObject* gun = GunOf(item.ptr)) {
                      s_lastPressedGun = gun;
                      s_lastPressedAt = GetTickCount64();
                  }
              });
    HookAfter("*:ReceiveBeginPlay", [](Object self, GUFunction*, void*) {
        if (!s_receiverClass) {
            Object donor = FindObject(kDonor);
            if (donor) s_receiverClass = donor.Get<GUClass*>("ReceiverClass");
        }
        if (s_receiverClass && self.Class() == s_receiverClass) QueueReceiver(self.ptr);
        else if (!s_dressedGuns.empty()) DressArrivedRound(self);
    });
    for (const char* f : {"ColorComponent:OnRep_ColorOption", "ColorComponent:ApplyColor_Server", "ColorComponent:ApplyColor"})
        HookAfter(f, [](Object cc, GUFunction*, void*) {
            Params o(cc, "GetOwner");
            o.Call();
            Object actor = o.Return<GUObject*>();
            if (!actor || !s_receiverClass || actor.Class() != s_receiverClass) return;
            if (IsDressed(actor) || !IsMarked(actor)) return;
            QueueReceiver(actor.ptr);
        });
    On(GML_EVENT_TICK, [](void*) {
        WatchLevel();
        ResolvePending();
        WatchLoadouts();
        WatchDressedGuns();
        ShowShotgunCategory();
        PoseRigs();
    });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) { NewLevel(); });
}

// ------------------------------------------------------------------ entry

GML_PLUGIN("com.remarrow.saiga12", "Saiga-12K", SAIGA_VERSION);

GML_AWAKE() {
    Message(SAIGA_MODEL_CREDIT);
    s_capacity = std::clamp(Config.Bind("Gun", "Capacity", 5, "Rounds per magazine (the Sb.5 holds 5).").Value(), 1, 100);
    s_fireRate = std::clamp(Config.Bind("Gun", "FireRate", 450, "Rounds per minute (the game's Benelli M4 cycles at 450).").Value(), 60, 1200);
    s_recoilScale = std::clamp(Config.Bind("Gun", "RecoilScale", 1.0f, "Recoil per shot, as a share of the game's Benelli M4's (1 = the same kick).").Value(), 0.1f, 5.0f);
    s_slugs = Config.Bind("Slug", "Enabled", true, "Fire slugs (false = the game's 9-pellet buckshot).").Value();
    s_slugDamage = std::clamp(Config.Bind("Slug", "Damage", 250.0f, "Damage at the muzzle (the game's 7.62x39 does 95, one buckshot pellet 35).").Value(), 1.0f, 2000.0f);
    s_slugVelocity = std::clamp(Config.Bind("Slug", "Velocity", 430.0f, "Muzzle velocity, m/s.").Value(), 100.0f, 1000.0f);
    s_slugMass = std::clamp(Config.Bind("Slug", "Mass", 28.0f, "Slug mass, grams.").Value(), 5.0f, 100.0f);
    s_offset[0] = Config.Bind("Visuals", "OffsetX", 0.0f, "cm, + = toward the muzzle (the model is fitted on the AK-74M's grip)").Value();
    s_offset[1] = Config.Bind("Visuals", "OffsetY", 0.0f, "cm, + = to the gun's right").Value();
    s_offset[2] = Config.Bind("Visuals", "OffsetZ", 0.0f, "cm, + = up").Value();
    s_fitAttachments = Config.Bind("Visuals", "FitAttachments", true, "Move the muzzle point and side-rail mount onto the Saiga's.").Value();
    s_ownMovingParts = Config.Bind("Visuals", "OwnMovingParts", true, "The Saiga's own bolt carrier, trigger, selector and mag release.").Value();
    s_ownMagazines = Config.Bind("Visuals", "OwnMagazines", true, "Magazines in a Saiga, and the spares you carry, look like the Sb.5.").Value();
    s_shotgunCategory = Config.Bind("Spawner", "ShotgunCategory", true,
        "Show the gun station's Shotguns category (the game ships it hidden), with the Saiga in it.").Value();

    HookAfter("BP_GameInstance_C:GetItems", [](Object gi, GUFunction*, void*) { AddGun(gi); RemoveBenelli(gi); });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) {
        if (Object gi = FindFirstOf("BP_GameInstance_C")) { AddGun(gi); RemoveBenelli(gi); }  // GetItems ran before we could see it
    });
    InstallVisuals();
    InstallSlugs();
    Log("loaded - adds '{}' to the Shotgun spawner: {} rounds, {} rpm, {}", kDisplayName, s_capacity, s_fireRate,
        s_slugs && s_fireRoundLocal ? "slugs" : "buckshot");
    return 0;
}

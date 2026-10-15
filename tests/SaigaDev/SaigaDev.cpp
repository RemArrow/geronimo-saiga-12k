// SaigaDev - automated in-game test of the Saiga-12K plugin. Not shipped; installed only for a run.
// Loads the team room, picks the Saiga at the gun station through the spawner's own menu event, holds the
// gun still in the air and checks: the Saiga meshes are on and the AK-74M's are hidden, its moving parts
// sit on the bones, the magazine is a 5-round 12 gauge Sb.5, rate / recoil / sound are set. Then it fires
// it through the game's own FireRoundLocal and logs every round that spawns (one 28 g slug per shot, not
// nine pellets), and that the 12 gauge round's defaults are buckshot again afterwards. Renders the gun with
// a scene capture of its own (lit and base colour) to [Test] ShotDir - the game's own rendering, no headset.
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <set>

using namespace gml;

GML_PLUGIN("com.remarrow.saigadev", "SaigaDev", "0.1.0", {{"com.remarrow.saiga12", "0.1.0", GML_DEPENDENCY_SOFT}});

struct Vec { double x, y, z; };
struct Rot { double pitch, yaw, roll; };

static const char* kItemPress =
    "BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature";
static const char* kConfirm =
    "BndEvt__WB_GunCustomization_WB_ConfirmWindow_K2Node_ComponentBoundEvent_0_OnConfirm__DelegateSignature";
static const char* kRound = "/Game/Weapons/Ammunition/12Gauge/BP_GunProjectile_12G_Buck.BP_GunProjectile_12G_Buck_C";

static std::string s_shotDir;
static bool s_gripOnly = false;
static std::string s_gripCurls;  // [Test] GripCurls: the curls to search at, e.g. "0.3,0.2,0.1,0"
static Object s_station, s_widget, s_saiga, s_gun, s_rt, s_cap, s_gear, s_benelli;
static int s_step = -1, s_wait = 0, s_fallbacks = 0, s_shotsWanted = 0;
static GUObject* s_before = nullptr;
static Vec s_holdAt{};
static Rot s_holdRot{};
static bool s_holding = false;
static int s_rounds = 0;  // rounds that began play since the last reset

static std::string TextOf(Object o, const char* prop) {
    char b[128] = "";
    if (void* t = o.PropPtr(prop)) API->TextToString(t, b, sizeof b);
    return b;
}
static std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}
static Object Pawn() {
    Params p(Lib("GameplayStatics"), "GetPlayerPawn");
    p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
    return p.Return<GUObject*>();
}
static Vec ActorLocation(Object a) {
    Params p(a, "K2_GetActorLocation");
    p.Call();
    return p.Return<Vec>();
}
static std::vector<Object> Components(Object actor, const char* cls) {
    std::vector<Object> out;
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass(cls)).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) out.push_back(((GUObject**)arr->Data)[i]);
    return out;
}
static bool Visible(Object c) {
    Params v(c, "IsVisible");
    v.Call();
    return v.Return<bool>();
}
static Vec ToActor(Object actor, Vec world) {
    Params gt(actor, "GetTransform");
    gt.Call();
    Params inv(Lib("KismetMathLibrary"), "InverseTransformLocation");
    GML_PropInfo ri;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &ri);
    std::memcpy(inv.Ptr("T"), gt.Ptr("ReturnValue"), ri.size);
    inv.Set("Location", world).Call();
    return inv.Return<Vec>();
}
static Vec CompLocation(Object c) {
    Params loc(c, "K2_GetComponentLocation");
    loc.Call();
    return loc.Return<Vec>();
}

// Hold the gun still in the air in front of our capture camera (the station eases it back to the table).
static void Hold() {
    if (!s_holding || !s_gun) return;
    Params mv(s_gun, "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", s_holdAt).Set("NewRotation", s_holdRot).Set("bSweep", false).Set("bTeleport", true).Call();
}

static void Freeze(Object gun) {
    if (Object root = gun.GetObj("StaticMeshComponent")) {
        Params p(root, "SetSimulatePhysics");
        p.Set("bSimulate", false).Call();
    }
}

static void EnsureCapture() {
    if (s_rt) return;
    Params rt(Lib("KismetRenderingLibrary"), "CreateRenderTarget2D");
    rt.SetObj("WorldContextObject", Object(API->WorldContext())).Set("Width", (int32_t)1600).Set("Height", (int32_t)800)
        .Set("Format", (uint8_t)2).Call();  // RTF_RGBA8
    s_rt = rt.Return<GUObject*>();
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Scale", Vec{1, 1, 1}).Call();
    GML_PropInfo xi;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &xi);
    Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
    b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", FindClass("SceneCapture2D")).Set("CollisionHandlingOverride", (uint8_t)1);
    std::memcpy(b.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), xi.size);
    b.Call();
    Object actor = b.Return<GUObject*>();
    Params f(Lib("GameplayStatics"), "FinishSpawningActor");
    f.SetObj("Actor", actor);
    std::memcpy(f.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), xi.size);
    f.Call();
    s_cap = actor.GetObj("CaptureComponent2D");
    s_cap.Set<GUObject*>("TextureTarget", s_rt.ptr);
    s_cap.SetBool("bCaptureEveryFrame", false);
    s_cap.Set("FOVAngle", 40.0f);
}

// The gun from its right and left side (and from above), lit (final colour) and as painted (base colour).
static void Capture(const char* file, bool both = true) {
    EnsureCapture();
    // Only the gun and its magazine (the player's body stands right next to it).
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);  // PRM_UseShowOnlyList
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        for (Object a : {s_gun, s_gun.GetObj("CurrentMagazine")})
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Params gt(s_gun, "GetActorRightVector");
    gt.Call();
    Vec r = gt.Return<Vec>();
    Params fw(s_gun, "GetActorForwardVector");
    fw.Call();
    Vec f = fw.Return<Vec>();
    Vec c = s_holdAt;
    c.x += f.x * 5; c.y += f.y * 5; c.z += f.z * 5;  // the middle of the Saiga
    struct View { const char* name; Vec at; } views[] = {
        {"right", {c.x + r.x * 150, c.y + r.y * 150, c.z + 10}},
        {"left", {c.x - r.x * 150, c.y - r.y * 150, c.z + 10}},
        {"top", {c.x + r.x * 20, c.y + r.y * 20, c.z + 150}},
    };
    for (auto& v : views) {
        if (!both && strcmp(v.name, "right")) continue;
        Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
        look.Set("Start", v.at).Set("Target", c).Call();
        Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
        mv.Set("NewLocation", v.at).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
        for (uint8_t source : {(uint8_t)0, (uint8_t)7}) {  // SCS_SceneColorHDR (lit, before exposure), SCS_BaseColor
            s_cap.Set("CaptureSource", source);
            Params cs(s_cap, "CaptureScene");
            cs.Call();
            std::string name = std::string(file) + "_" + v.name + (source == 0 ? "_lit" : "_base") + ".png";
            Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
            ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
                .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(name)).Call();
        }
    }
    Log("SHOT {}", file);
}

// The Saiga's parts (DynamicMeshComponents, in the order the plugin adds them) and how each is posed.
static void LogParts(const char* when) {
    static const char* names[] = {"body", "muzzle", "carrier", "bolt", "selector", "trigger", "mag release"};
    auto parts = Components(s_gun, "DynamicMeshComponent");
    Log("ACTION [{}] FireMode {} CurrentTrigger {:.2f} release {} bolt progress {:.2f}", when, s_gun.Get<uint8_t>("FireMode"),
        s_gun.Get<float>("CurrentTrigger"), s_gun.GetBool("bIsMagReleasePressed"), s_gun.Get<float>("CurrentBoltProgress"));
    for (size_t i = 2; i < parts.size() && i < std::size(names); i++) {
        Vec l = parts[i].Get<Vec>("RelativeLocation");
        Rot r = parts[i].Get<Rot>("RelativeRotation");
        Log("ACTION   {:<12} location ({:7.3f},{:7.3f},{:7.3f}) rotation pitch {:7.2f} yaw {:6.2f} roll {:7.2f}", names[i], l.x, l.y, l.z,
            r.pitch, r.yaw, r.roll);
    }
}

// The charging handle's grab box (and the hand poses under it) against the Saiga's knob, which rides the
// carrier (the plugin's third part).
static void LogHandle(Object gun, const char* when) {
    Object h = gun.GetObj("ChargingHandle");
    if (!h) { Error("HANDLE [{}] no ChargingHandle on {}", when, gun.Name()); return; }
    auto parts = Components(gun, "DynamicMeshComponent");
    Vec carrier = parts.size() > 2 ? parts[2].Get<Vec>("RelativeLocation") : Vec{};
    Vec box = ToActor(gun, CompLocation(h)), rel = h.Get<Vec>("RelativeLocation");
    Vec knob{8.457 + carrier.x, 4.146 + carrier.y, 0.956 + carrier.z};  // the grab spot on the Saiga's knob, carried along
    double off = std::sqrt((box.x - knob.x) * (box.x - knob.x) + (box.y - knob.y) * (box.y - knob.y) + (box.z - knob.z) * (box.z - knob.z));
    Log("HANDLE [{}] progress {:.2f} box at ({:.3f},{:.3f},{:.3f}) relative ({:.3f},{:.3f},{:.3f}); carrier {:.2f} -> knob spot ({:.3f},{:.3f},{:.3f}): {:.1f} mm apart",
        when, h.Get<float>("CurrentProgress"), box.x, box.y, box.z, rel.x, rel.y, rel.z, carrier.x, knob.x, knob.y, knob.z, off * 10);
    for (auto& c : Components(gun, "SceneComponent")) {
        std::string n = c.Name();
        if (n != "HandSocket" && n != "HandSocket1" && n != "HandSocket2" && n != "HandSocket_ChargingHandle") continue;
        Vec l = ToActor(gun, CompLocation(c));
        Log("HANDLE   {:<26} at ({:.3f},{:.3f},{:.3f}), box + ({:.3f},{:.3f},{:.3f})", n, l.x, l.y, l.z, l.x - box.x, l.y - box.y, l.z - box.z);
    }
}
// Pulls the handle through its own SetProgress, as the hand pulling it does (unlocked first: a gun spawned on
// safe holds it at the safety's stop).
static void SetHandle(float progress) {
    Object h = s_gun.GetObj("ChargingHandle");
    if (!h) return;
    if (h.GetBool("bIsLocked")) {
        Params u(h, "UnlockHandle");
        u.Call();
    }
    Params p(h, "SetProgress");
    p.Set("Progress", progress).Call();
}
// The handle's unreflected fields (past GrippableBoxComponent's), as floats: where its copy of the settings is.
static void DumpHandle(Object gun) {
    Object h = gun.GetObj("ChargingHandle");
    if (!h) return;
    int from = API->StructSize((GUStruct*)FindClass("GrippableBoxComponent")), to = API->StructSize(h.Struct());
    std::string line;
    for (int o = from; o + 4 <= to; o += 4) {
        float f = *(float*)((uint8_t*)h.ptr + o);
        if (f != 0 && std::isfinite(f) && std::fabs(f) > 1e-3 && std::fabs(f) < 1e4) line += std::format(" +{:X}={:.4g}", o, f);
    }
    Log("HANDLE fields 0x{:X}..0x{:X}:{}", from, to, line);
    std::string hb;
    for (int o = 0x658; o < 0x688; o++) hb += std::format("{:02X}{}", ((uint8_t*)h.ptr)[o], (o & 3) == 3 ? " " : "");
    Log("HANDLE bytes +658: {}", hb);
    GML_PropInfo si{};
    GFProperty* hs = API->FindProperty(gun.Struct(), "ChargingHandleSettings");
    if (!hs || !API->GetPropertyInfo(hs, &si) || !si.structType) return;
    std::string rb;
    for (int o = 0; o < 0x2C; o++) rb += std::format("{:02X}{}", ((uint8_t*)gun.ptr)[si.offset + o], (o & 3) == 3 ? " " : "");
    Log("HANDLE receiver ChargingHandleSettings (+0x{:X}): {}", si.offset, rb);
}

// ---- the hand on the charging handle: the game's XR hand mesh, placed and posed by each of the handle's hand
// sockets as VRExpansion does (GetMeshRelativeTransform on the box the sockets hang on; GetBlendedPoseSnapShot
// for the fingers; a left hand is the right mesh with the socket's mirrored scale)
static const char* kHandMesh = "/Game/Characters/Common/Hands/SKM_MannyXR_right.SKM_MannyXR_right";
struct XfD { double q[4]; double t[3]; double pad0; double s[3]; double pad1; };
static XfD Then(const XfD& a, const XfD& b) {  // a, then b (FTransform a * b)
    const double *p = b.q, *q = a.q;
    XfD r{};
    r.q[3] = p[3] * q[3] - p[0] * q[0] - p[1] * q[1] - p[2] * q[2];
    r.q[0] = p[3] * q[0] + p[0] * q[3] + p[1] * q[2] - p[2] * q[1];
    r.q[1] = p[3] * q[1] - p[0] * q[2] + p[1] * q[3] + p[2] * q[0];
    r.q[2] = p[3] * q[2] + p[0] * q[1] - p[1] * q[0] + p[2] * q[3];
    double v[3] = {a.t[0] * b.s[0], a.t[1] * b.s[1], a.t[2] * b.s[2]};
    double tx = 2 * (p[1] * v[2] - p[2] * v[1]), ty = 2 * (p[2] * v[0] - p[0] * v[2]), tz = 2 * (p[0] * v[1] - p[1] * v[0]);
    r.t[0] = v[0] + p[3] * tx + (p[1] * tz - p[2] * ty) + b.t[0];
    r.t[1] = v[1] + p[3] * ty + (p[2] * tx - p[0] * tz) + b.t[1];
    r.t[2] = v[2] + p[3] * tz + (p[0] * ty - p[1] * tx) + b.t[2];
    for (int i = 0; i < 3; i++) r.s[i] = a.s[i] * b.s[i];
    return r;
}
struct HandView { const char* socket; bool right; const char* name; bool onMag = false; };
static const HandView kHands[] = {
    {"HandSocket_ChargingHandle", true, "handle_R"}, {"HandSocket_ChargingHandle", false, "handle_L"},
    {"HandSocket2", true, "socket2_R"},              {"HandSocket", false, "socket_L"},
    {"HandSocket1", false, "socket1_L"},
    {"Magazine Hand Socket", false, "mag_L", true},  {"Magazine Hand Socket", true, "mag_R", true},
    {"SecondaryHandSocket", false, "second_L"},      {"PrimaryHandSocket", true, "primary_R"},
};
static std::vector<Object> s_hands;
static std::vector<std::vector<uint64_t>> s_handBones;
static size_t s_handShot = 0;
// The Sb.5 seated in the gun (actor space, tools\prep)
static const Vec kMagLo{-0.21, -1.63, -17.78}, kMagHi{12.38, 1.63, -1.11};

static std::string NameStr(uint64_t n) {
    char b[96] = "";
    API->NameToString(n, b, sizeof b);
    return b;
}
static std::vector<Object> s_handSockets;
static Object s_target;  // a hidden skeletal hand: sockets posed by bone deltas build on its reference pose

// The socket's finger pose with its bone deltas turned by `curl` of their angle (1 = as made, 0 = the open
// reference hand), in the hand's component space; applied to the hand mesh if `apply`.
static std::map<uint64_t, XfD> PoseHand(Object hand, Object sock, double curl, bool apply) {
    struct Pair { uint64_t bone; uint64_t pad; double q[4]; uint8_t rest[0x10]; };
    static_assert(sizeof(Pair) == 0x40);
    auto* deltas = (GML_TArray*)sock.PropPtr("CustomPoseDeltas");
    std::vector<Pair> saved;
    if (deltas && curl != 1.0)
        for (int i = 0; i < deltas->Num; i++) {
            Pair& p = ((Pair*)deltas->Data)[i];
            saved.push_back(p);
            double w = std::clamp(std::fabs(p.q[3]), 0.0, 1.0), sgn = p.q[3] < 0 ? -1 : 1, half = std::acos(w), s = std::sin(half);
            if (s < 1e-9) continue;
            double k = std::sin(half * curl) / s;
            for (int j = 0; j < 3; j++) p.q[j] = p.q[j] * sgn * k;
            p.q[3] = std::cos(half * curl);
        }
    Params ps(sock, "GetBlendedPoseSnapShot");
    ps.SetObj("TargetMesh", s_target).Set("bSkipRootBone", false).Set("bFlipHand", false).Call();
    for (size_t i = 0; i < saved.size(); i++) ((Pair*)deltas->Data)[i] = saved[i];
    auto* snap = (uint8_t*)ps.Ptr("PoseSnapshot");
    auto* xfs = (GML_TArray*)snap;
    auto* names = (GML_TArray*)(snap + 0x10);
    std::map<uint64_t, XfD> comp;
    for (int i = 0; ps.Return<bool>() && i < xfs->Num && i < names->Num; i++) {
        uint64_t n = ((uint64_t*)names->Data)[i];
        XfD local = ((XfD*)xfs->Data)[i];
        Params gp(hand, "GetParentBone");
        *(uint64_t*)gp.Ptr("BoneName") = n;
        gp.Call();
        auto parent = comp.find(gp.Return<uint64_t>());
        XfD c = parent != comp.end() ? Then(local, parent->second) : local;
        comp[n] = c;
        if (!apply) continue;
        Params sb(hand, "SetBoneTransformByName");
        *(uint64_t*)sb.Ptr("BoneName") = n;
        std::memcpy(sb.Ptr("InTransform"), &c, sizeof c);
        sb.Set("BoneSpace", (uint8_t)1).Call();  // ComponentSpace
    }
    return comp;
}
static void BuildHands() {
    Object mesh = LoadObject(kHandMesh), box = s_gun.GetObj("ChargingHandle");
    if (!mesh || !box) { Error("HAND no hand mesh ({}) or no ChargingHandle", mesh.Name()); return; }
    std::map<std::string, Object> sockets, magSockets;
    for (auto& c : Components(s_gun, "SceneComponent")) sockets[c.Name()] = c;
    if (Object mag = s_gun.GetObj("CurrentMagazine"))
        for (auto& c : Components(mag, "SceneComponent")) magSockets[c.Name()] = c;
    // a hidden skeletal hand: sockets posed by bone deltas (no animation) build their pose on its reference pose
    Object target;
    {
        Params add(s_gun, "AddComponentByClass");
        XfD id{{0, 0, 0, 1}, {0, 0, 0}, 0, {1, 1, 1}, 0};
        add.Set("Class", FindClass("SkeletalMeshComponent")).Set("bManualAttachment", false).Set("bDeferredFinish", false);
        std::memcpy(add.Ptr("RelativeTransform"), &id, sizeof id);
        add.Call();
        target = add.Return<GUObject*>();
        Params sm(target, "SetSkinnedAssetAndUpdate");
        sm.SetObj("NewMesh", mesh).Set("bReinitPose", true).Call();
        Params v(target, "SetVisibility");
        v.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();
    }
    for (auto& hv : kHands) {
        Object sock = (hv.onMag ? magSockets : sockets)[hv.socket];
        if (!sock) { Error("HAND no {}", hv.socket); s_hands.push_back(Object()); s_handSockets.push_back(Object()); s_handBones.push_back({}); continue; }
        Params gp0(sock, "GetAttachParent");  // the hand is placed relative to what the socket hangs on
        gp0.Call();
        box = gp0.Return<GUObject*>();
        Params add(s_gun, "AddComponentByClass");
        XfD id{{0, 0, 0, 1}, {0, 0, 0}, 0, {1, 1, 1}, 0};
        add.Set("Class", FindClass("PoseableMeshComponent")).Set("bManualAttachment", true).Set("bDeferredFinish", false);
        std::memcpy(add.Ptr("RelativeTransform"), &id, sizeof id);
        add.Call();
        Object hand = add.Return<GUObject*>();
        Params at(hand, "K2_AttachToComponent");
        at.SetObj("Parent", box).Set("LocationRule", (uint8_t)0).Set("RotationRule", (uint8_t)0).Set("ScaleRule", (uint8_t)0)
            .Set("bWeldSimulatedBodies", false).Call();
        Params sm(hand, "SetSkinnedAssetAndUpdate");
        sm.SetObj("NewMesh", mesh).Set("bReinitPose", true).Call();
        Params mr(sock, "GetMeshRelativeTransform");
        mr.Set("bIsRightHand", hv.right).Set("bUseParentScale", true).Set("bUseMirrorScale", true).Call();
        XfD rel;
        std::memcpy(&rel, mr.Ptr("ReturnValue"), sizeof rel);
        Params sr(hand, "K2_SetRelativeTransform");
        std::memcpy(sr.Ptr("NewTransform"), &rel, sizeof rel);
        sr.Set("bSweep", false).Set("bTeleport", true).Call();
        // the fingers: the socket's pose, bone by bone in component space
        s_target = target;
        auto comp = PoseHand(hand, sock, 1.0, true);
        Log("HAND {:<10} {} ({} hand): placement ({:.2f},{:.2f},{:.2f}) scale ({:.0f},{:.0f},{:.0f}) from the box, pose {} with {} bones", hv.name, hv.socket,
            hv.right ? "right" : "left", rel.t[0], rel.t[1], rel.t[2], rel.s[0], rel.s[1], rel.s[2], comp.empty() ? "FAILED" : "ok", comp.size());
        s_handSockets.push_back(sock);
        s_hands.push_back(hand);
        s_handBones.push_back({});
        for (auto& [n, _] : comp) s_handBones.back().push_back(n);
        if (s_hands.size() == 1) {
            std::string bones;
            for (auto n : s_handBones.back()) bones += " " + NameStr(n);
            Log("HAND bones:{}", bones);
        }
    }
}
// The hand as spheres (actor space): each finger from its first knuckle to its tip (the tip carried on past the
// last joint by 0.85 of the middle segment), 8.5 mm thick (thumb 10); the palm along the metacarpals, 13 mm.
struct Ball { Vec c; double r; const char* part; };
static Vec BoneAt(Object hand, const char* bone) {
    Params gl(hand, "GetSocketLocation");
    *(uint64_t*)gl.Ptr("InSocketName") = API->MakeName(bone);
    gl.Call();
    return ToActor(s_gun, gl.Return<Vec>());
}
static std::vector<Ball> HandBalls(const std::function<Vec(const char*)>& at, double thick = 1.0) {
    std::vector<Ball> balls;
    auto seg = [&](Vec a, Vec b, double r, const char* part) {
        for (int k = 0; k <= 4; k++) balls.push_back({{a.x + (b.x - a.x) * k / 4, a.y + (b.y - a.y) * k / 4, a.z + (b.z - a.z) * k / 4}, r, part});
    };
    Vec wrist = at("hand_r");
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky"}) {
        bool thumb = !strcmp(f, "thumb");
        Vec j1 = at(std::format("{}_01_r", f).c_str()), j2 = at(std::format("{}_02_r", f).c_str()), j3 = at(std::format("{}_03_r", f).c_str());
        Vec tip{j3.x + (j3.x - j2.x) * 0.85, j3.y + (j3.y - j2.y) * 0.85, j3.z + (j3.z - j2.z) * 0.85};
        double r = (thumb ? 0.5 : 0.425) * thick;
        seg(j1, j2, r, f); seg(j2, j3, r, f); seg(j3, tip, r * 0.9, f);
        if (!thumb) seg(wrist, j1, 0.65 * thick, "palm");
    }
    return balls;
}
static std::vector<Ball> HandBalls(Object hand) { return HandBalls([&](const char* b) { return BoneAt(hand, b); }); }
// The Sb.5 as the box it is: tilted 18 degrees (nose down) about Y, 8.4 cm front to back, 3.26 wide, 17.3 long
// (fitted to tools\prep's mesh). u runs front-to-back along its top, w along its length.
static constexpr double kMagTilt = 18 * 3.14159265358979 / 180, kMagU0 = -1.13, kMagU1 = 7.27, kMagW0 = -18.41, kMagW1 = -1.15, kMagHalf = 1.63;
static Vec ToMag(Vec p) { return {p.x * std::cos(kMagTilt) + p.z * std::sin(kMagTilt), p.y, -p.x * std::sin(kMagTilt) + p.z * std::cos(kMagTilt)}; }
static Vec FromMag(Vec q) { return {q.x * std::cos(kMagTilt) - q.z * std::sin(kMagTilt), q.y, q.x * std::sin(kMagTilt) + q.z * std::cos(kMagTilt)}; }
// How far a ball reaches into the magazine (cm, positive = in), moved by d (actor space). The tilted box is cut
// level where the magazine enters the magazine well (its top, Z -1.11): above that is the receiver.
static double Into(const Ball& b, Vec d) {
    Vec a{b.c.x + d.x, b.c.y + d.y, b.c.z + d.z}, p = ToMag(a);
    double ou = (std::max)({kMagU0 - p.x, p.x - kMagU1, 0.0}), oy = (std::max)({-kMagHalf - p.y, p.y - kMagHalf, 0.0}),
           ow = (std::max)({kMagW0 - p.z, p.z - kMagW1, 0.0});
    double out = std::sqrt(ou * ou + oy * oy + ow * ow);
    if (out == 0) out = -(std::min)({p.x - kMagU0, kMagU1 - p.x, p.y + kMagHalf, kMagHalf - p.y, p.z - kMagW0, kMagW1 - p.z});
    return b.r - (std::max)(out, a.z - kMagHi.z);
}

// ---- a grip that fits the Sb.5: the magazine socket's hand, its fingers opened by some share of their curl and
// the whole hand moved (in the magazine's frame), the best that is 1.5 mm clear and closest to the magazine.
struct MagFit { double curl = 1; Vec move{}; double score = 1e9, worst = 0, contact = 0; };
static MagFit s_magFit;
static void MagGripSearch(size_t i) {
    Object hand = s_hands[i], sock = s_handSockets[i];
    Params cw(hand, "K2_GetComponentToWorld");
    cw.Call();
    XfD world;
    std::memcpy(&world, cw.Ptr("ReturnValue"), sizeof world);
    MagFit best;
    for (double curl : {1.0, 0.85, 0.7, 0.55, 0.4, 0.25, 0.1}) {
        auto comp = PoseHand(hand, sock, curl, false);
        std::map<std::string, Vec> joints;
        for (auto& [n, c] : comp) {
            XfD w = Then(c, world);
            joints[NameStr(n)] = ToActor(s_gun, {w.t[0], w.t[1], w.t[2]});
        }
        auto balls = HandBalls([&](const char* b) { return joints[b]; });
        MagFit top;
        for (double du = -4; du <= 4.001; du += 0.25)
            for (double dy = -4; dy <= 4.001; dy += 0.25)
                for (double dw = -4; dw <= 4.001; dw += 0.5) {
                    Vec d = FromMag({du, dy, dw});
                    double worst = -1e9;
                    std::vector<double> gaps;
                    for (auto& b : balls) {
                        double v = Into(b, d);
                        worst = (std::max)(worst, v);
                        if (strcmp(b.part, "thumb")) gaps.push_back(-v);
                    }
                    if (worst > -0.15) continue;
                    std::partial_sort(gaps.begin(), gaps.begin() + 16, gaps.end());
                    double contact = 0;
                    for (int k = 0; k < 16; k++) contact += gaps[k] / 16;
                    double score = contact + 0.04 * std::sqrt(du * du + dy * dy + dw * dw) + 0.15 * (1 - curl);
                    if (score < top.score) top = {curl, d, score, worst, contact};
                }
        Log("MAGFIT curl {:.2f}: best move ({:.2f},{:.2f},{:.2f}) - clear by {:.1f} mm, fingers and palm {:.1f} mm from the magazine (score {:.3f})", curl,
            top.move.x, top.move.y, top.move.z, -top.worst * 10, top.contact * 10, top.score);
        if (top.score < best.score) best = top;
    }
    s_magFit = best;
    // show it: the hand posed with that curl and moved
    PoseHand(hand, sock, best.curl, true);
    Params mv(hand, "K2_AddWorldOffset");
    mv.Set("DeltaLocation", best.move).Set("bSweep", false).Set("bTeleport", true).Call();
    // the move as the socket's parent (the magazine) sees it
    Object parent;
    {
        Params gp(sock, "GetAttachParent");
        gp.Call();
        parent = gp.Return<GUObject*>();
    }
    Params pw(parent, "K2_GetComponentToWorld");
    pw.Call();
    Params inv(Lib("KismetMathLibrary"), "InverseTransformDirection");
    std::memcpy(inv.Ptr("T"), pw.Ptr("ReturnValue"), sizeof(XfD));
    inv.Set("Direction", best.move).Call();
    Vec local = inv.Return<Vec>();
    Log("MAGFIT BEST curl {:.2f}, move ({:.2f},{:.2f},{:.2f}) in the gun = ({:.3f},{:.3f},{:.3f}) on the magazine's {} - clear by {:.1f} mm, {:.1f} mm from it",
        best.curl, best.move.x, best.move.y, best.move.z, local.x, local.y, local.z, parent.Name(), -best.worst * 10, best.contact * 10);
}
static bool s_magFitDone = true;  // [Test] MagGripSearch = true searches again (on top of the plugin's fit)

static void LogHandInMag(size_t i) {
    Object hand = s_hands[i];
    if (!hand) return;
    auto balls = HandBalls(hand);
    auto worst = [&](Vec d, const char** part) {
        double w = -1e9;
        for (auto& b : balls)
            if (double v = Into(b, d); v > w) { w = v; if (part) *part = b.part; }
        return w;
    };
    const char* part = "";
    double now = worst({0, 0, 0}, &part);
    std::string parts;
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "palm"}) {
        double w = -1e9;
        for (auto& b : balls)
            if (!strcmp(b.part, f)) w = (std::max)(w, Into(b, {0, 0, 0}));
        parts += std::format(" {} {:.0f}", f, w * 10);
    }
    // the smallest move of the hand that keeps it 3 mm clear, along each axis
    std::string moves;
    for (auto [name, dir] : std::initializer_list<std::pair<const char*, Vec>>{{"+Y out", {0, 1, 0}}, {"+Z up", {0, 0, 1}}, {"-Z down", {0, 0, -1}},
                                                                             {"+X fwd", {1, 0, 0}}, {"-X back", {-1, 0, 0}}}) {
        double need = -1;
        for (double s = 0; s <= 6.0 && need < 0; s += 0.05)
            if (worst({dir.x * s, dir.y * s, dir.z * s}, nullptr) <= -0.3) need = s;
        moves += need < 0 ? std::format(" {} >6 cm", name) : std::format(" {} {:.1f} cm", name, need);
    }
    Log("HAND {:<10} reaches {:.0f} mm into the magazine (deepest: {}; by part, mm:{}); clear with{}", kHands[i].name, now * 10, part, parts, moves);
}

// The receiver close up from its right side, base colour, only the gun and its magazine drawn.
static void CaptureZoom(const char* file) {
    EnsureCapture();
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        for (Object a : {s_gun, s_gun.GetObj("CurrentMagazine")})
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Vec c{s_holdAt.x - 2, s_holdAt.y, s_holdAt.z - 1};  // the receiver (the gun is held unrotated)
    Vec from{c.x, c.y + 55, c.z + 4};
    Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
    look.Set("Start", from).Set("Target", c).Call();
    Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", from).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
    s_cap.Set("CaptureSource", (uint8_t)7);  // SCS_BaseColor
    Params cs(s_cap, "CaptureScene");
    cs.Call();
    Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
    ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
        .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(std::string(file) + ".png")).Call();
    Log("SHOT {}", file);
}

// From `from` toward `at` (both actor space; the gun is held unrotated), base colour, only the gun and its magazine.
static void CaptureFrom(const std::string& file, Vec from, Vec at) {
    EnsureCapture();
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        for (Object a : {s_gun, s_gun.GetObj("CurrentMagazine")})
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Vec f{s_holdAt.x + from.x, s_holdAt.y + from.y, s_holdAt.z + from.z}, t{s_holdAt.x + at.x, s_holdAt.y + at.y, s_holdAt.z + at.z};
    Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
    look.Set("Start", f).Set("Target", t).Call();
    Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", f).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
    s_cap.Set("CaptureSource", (uint8_t)7);  // SCS_BaseColor
    Params cs(s_cap, "CaptureScene");
    cs.Call();
    Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
    ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
        .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(file + ".png")).Call();
}

static void LogGun(const char* when) {
    Object g = s_gun;
    Log("GUN [{}] {} class {} BulletData {} rpm {:.0f}/{:.0f} mode {}", when, g.Name(), Object((GUObject*)g.Class()).Name(),
        Object((GUObject*)g.Get<GUClass*>("BulletData")).Name(), g.Get<float>("FireRateRPMMin"), g.Get<float>("FireRateRPMMax"),
        g.Get<uint8_t>("FireMode"));
    Vec rps = g.Get<Vec>("RotationPerShot"), lps = g.Get<Vec>("LocationPerShot");
    Log("GUN   recoil per shot rot ({:.2f},{:.2f},{:.2f}) loc ({:.2f},{:.2f},{:.2f}); sound {} shot {}", rps.x, rps.y, rps.z, lps.x, lps.y,
        lps.z, g.GetObj("AudioData").Name(), Object(g.GetObj("AudioData").Get<GUObject*>("ShotSound")).Name());
    Log("GUN   rounds: unspent {} spent {} proxy {}", g.GetObj("UnspentRoundMesh").Name(), g.GetObj("SpentRoundMesh").Name(),
        g.GetObj("ProxyRound").GetObj("StaticMesh").Name());
    for (const char* cls : {"StaticMeshComponent", "DynamicMeshComponent", "PoseableMeshComponent"})
        for (auto& c : Components(g, cls)) {
            Params sn(c, "GetAttachSocketName");
            sn.Call();
            char sock[64] = "";
            API->NameToString(sn.Return<uint64_t>(), sock, sizeof sock);
            Vec l = ToActor(g, CompLocation(c));
            Log("GUN   {} [{}] visible {} socket '{}' at ({:.2f},{:.2f},{:.2f})", c.Name(), Object((GUObject*)c.Class()).Name(), Visible(c), sock,
                l.x, l.y, l.z);
        }
    for (auto& c : Components(g, "SceneComponent"))
        if (c.Name() == "Fire Location" || c.Name() == "AK_Dovetail") {
            Vec l = ToActor(g, CompLocation(c));
            Log("GUN   attach point {} at ({:.3f},{:.3f},{:.3f})", c.Name(), l.x, l.y, l.z);
        }
    Object mag = g.GetObj("CurrentMagazine");
    Log("MAG   {} [{}] capacity {} ammo {} BulletData {}", mag.Name(), Object((GUObject*)mag.Class()).Name(), mag.Get<int32_t>("AmmoCapacity"),
        mag.Get<int32_t>("CurrentAmmo"), Object((GUObject*)mag.Get<GUClass*>("BulletData")).Name());
    if (mag)
        for (const char* cls : {"StaticMeshComponent", "DynamicMeshComponent", "InstancedStaticMeshComponent"})
            for (auto& c : Components(mag, cls)) Log("MAG     {} [{}] visible {}", c.Name(), Object((GUObject*)c.Class()).Name(), Visible(c));
}

static void LogRoundDefaults(const char* when) {
    GUClass* cls = LoadClass(kRound);
    Object d = DefaultObject(cls);
    Log("ROUND DEFAULTS [{}] Shotgun {} ShotCount {} Mass {:.4f} Diameter {:.2f} velocity {:.0f}-{:.0f} pooling {} DamageCurve {}", when,
        d.GetBool("Shotgun"), d.Get<int32_t>("ShotCount"), d.Get<float>("Mass"), d.Get<float>("Diameter"), d.Get<float>("MuzzleVelocityMin"),
        d.Get<float>("MuzzleVelocityMax"), d.GetBool("EnablePooling"), d.GetObj("DamageCurve").Name());
}

// Open the Shotguns page as the VR pointer does (its category button), so its items exist.
static void OpenShotguns() {
    Object button = s_widget.GetObj("CategoryBtn_Shotguns");
    Params gv(button, "GetVisibility");
    gv.Call();
    Log("CATEGORY button {} '{}' visibility {} (0 = Visible; the game ships it 2 = Hidden)", button.Name(), TextOf(button, "Title"),
        gv.Return<uint8_t>());
    Params cat(s_widget, "BndEvt__WB_GunCustomization_CategoryBtn_Shotguns_K2Node_ComponentBoundEvent_27_OnPressed__DelegateSignature");
    cat.Call();
    Params sw(s_widget, "ShowWeapons");
    sw.Set("Type", (uint8_t)3).Call();
}

static bool Select(Object asset) {
    s_before = s_station.GetObj("CurrentReceiver").ptr;
    Object item;
    std::string seen;
    for (auto& it : FindAllOf("WB_CustomizationItemGun_C")) {
        seen += TextOf(it.GetObj("WeaponItem"), "DisplayName") + ", ";
        if (it.GetObj("WeaponItem").ptr == asset.ptr) item = it;
    }
    Log("menu items: {}", seen);
    if (!item) { Error("no menu item for '{}'", TextOf(asset, "DisplayName")); return false; }
    Params press(item, kItemPress);
    press.Call();
    Log("pressed menu item {} for '{}'", item.Name(), TextOf(asset, "DisplayName"));
    return true;
}

// ---- the player's own left hand taking a spare Sb.5 out of a pouch through the game's own grip
// (AVRPlayer::TryGrip), held up in front of the head: the hand you see in VR, against the Sb.5
static Object s_gripCtrl, s_gripMag, s_gripCam, s_xrHand;
static Vec s_gripAt;
static bool s_gripHold = false;
static XfD CompXf(Object c) {
    Params p(c, "K2_GetComponentToWorld");
    p.Call();
    XfD x;
    std::memcpy(&x, p.Ptr("ReturnValue"), sizeof x);
    return x;
}
static Vec InvXfLoc(const XfD& t, Vec p) {
    Params inv(Lib("KismetMathLibrary"), "InverseTransformLocation");
    std::memcpy(inv.Ptr("T"), &t, sizeof t);
    inv.Set("Location", p).Call();
    return inv.Return<Vec>();
}
static void HoldGrip() {
    if (!s_gripHold || !s_gripCtrl) return;
    Params mv(s_gripCtrl, "K2_SetWorldLocationAndRotation");
    mv.Set("NewLocation", s_gripAt).Set("NewRotation", Rot{0, 0, 0}).Set("bSweep", false).Set("bTeleport", true).Call();
}
// From `from` toward `at` (world), base colour, only these actors.
static void CaptureWorld(const std::string& file, std::initializer_list<Object> actors, Vec from, Vec at) {
    EnsureCapture();
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        for (Object a : actors)
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
    look.Set("Start", from).Set("Target", at).Call();
    Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", from).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
    s_cap.Set("CaptureSource", (uint8_t)7);
    Params cs(s_cap, "CaptureScene");
    cs.Call();
    Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
    ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
        .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(file + ".png")).Call();
}
// Joints of a hand in the Sb.5's frame (the plugin's mesh on that magazine: the OBJ's coordinates, where the
// magazine model above is defined).
static void LogGripInMag(const char* who, Object mesh, const char* side, Object mag) {
    auto dm = Components(mag, "DynamicMeshComponent");
    if (dm.empty()) { Error("GRIP {} is not an Sb.5", mag.Name()); return; }
    XfD sb5 = CompXf(dm[0]);
    auto at = [&](const char* bone) {
        std::string b = bone;
        if (b.size() > 2 && b.substr(b.size() - 2) == "_r") b = b.substr(0, b.size() - 2) + side;
        Params gl(mesh, "GetSocketLocation");
        *(uint64_t*)gl.Ptr("InSocketName") = API->MakeName(b.c_str());
        gl.Call();
        return InvXfLoc(sb5, gl.Return<Vec>());
    };
    auto balls = HandBalls(at);
    std::string parts;
    double worst = -1e9;
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "palm"}) {
        double w = -1e9;
        for (auto& b : balls)
            if (!strcmp(b.part, f)) w = (std::max)(w, Into(b, {0, 0, 0}));
        worst = (std::max)(worst, w);
        parts += std::format(" {} {:.0f}", f, w * 10);
    }
    Vec wrist = at("hand_r"), tip = at("middle_03_r");
    Log("GRIP {}: reaches {:.0f} mm into the Sb.5 (mm by part:{}); wrist at ({:.1f},{:.1f},{:.1f}), middle finger tip joint ({:.1f},{:.1f},{:.1f}) in the Sb.5's frame",
        who, worst * 10, parts, wrist.x, wrist.y, wrist.z, tip.x, tip.y, tip.z);
}

static constexpr double kPluginCurl = 0.8;  // the plugin's kMagGripCurl: the Sb.5 grip's fingers at 80% of their curl
static double s_curNow = kPluginCurl;
static std::vector<double> s_curls;
static size_t s_curlIdx = 0;
static std::vector<std::vector<Ball>> s_curlBalls;

// Turns each of the socket's finger deltas by k times its angle.
static void ScaleDeltas(Object sock, double k) {
    struct Pair { uint64_t bone, pad; double q[4]; uint8_t rest[0x10]; };
    auto* d = (GML_TArray*)sock.PropPtr("CustomPoseDeltas");
    for (int i = 0; d && i < d->Num && k != 1.0; i++) {
        double* q = ((Pair*)d->Data)[i].q;
        double sgn = q[3] < 0 ? -1 : 1, half = std::acos(std::clamp(std::fabs(q[3]), 0.0, 1.0)), s = std::sin(half);
        if (s < 1e-9) continue;
        double f = std::sin(half * k) / s;
        for (int j = 0; j < 3; j++) q[j] *= sgn * f;
        q[3] = std::cos(half * k);
    }
}
// A VRExpansion slot grip, as the game's own: the magazine's hand socket onto the controller.
static bool SlotGrip(Object ctrl, Object mag) {
    Object sock = mag.GetObj("HandSocket");
    Params hs(sock, "GetHandSocketTransform");
    hs.SetObj("QueryController", ctrl).Set("bIgnoreOnlySnapMesh", false).Call();
    Params gt(mag, "GetTransform");
    gt.Call();
    Params rel(Lib("KismetMathLibrary"), "MakeRelativeTransform");
    std::memcpy(rel.Ptr("A"), gt.Ptr("ReturnValue"), sizeof(XfD));
    std::memcpy(rel.Ptr("RelativeTo"), hs.Ptr("ReturnValue"), sizeof(XfD));
    rel.Call();
    Params g(ctrl, "GripObjectByInterface");
    g.SetObj("ObjectToGrip", mag);
    std::memcpy(g.Ptr("WorldOffset"), rel.Ptr("ReturnValue"), sizeof(XfD));
    *(uint64_t*)g.Ptr("OptionalSnapToSocketName") = API->MakeName(sock.Name().c_str());
    g.Set("bWorldOffsetIsRelative", true).Set("bIsSlotGrip", true).Call();
    return g.Return<bool>();
}
// The player body's left hand as balls in the Sb.5's frame (a gloved hand: 15% thicker than the XR hand's).
static std::string s_gripSide = "_l";  // the hand holding it: _l, then _r
static std::vector<Ball> BodyBalls(Object body, Object mag) {
    XfD sb5 = CompXf(Components(mag, "DynamicMeshComponent")[0]);
    return HandBalls([&](const char* bone) {
        std::string b = bone;
        b = b.substr(0, b.size() - 2) + s_gripSide;
        Params gl(body, "GetSocketLocation");
        *(uint64_t*)gl.Ptr("InSocketName") = API->MakeName(b.c_str());
        gl.Call();
        return InvXfLoc(sb5, gl.Return<Vec>());
    }, 1.15);
}
// The smallest move of a rigid hand (balls in the Sb.5's frame) that keeps it 1.5 mm out, closest to the magazine.
static MagFit ShiftSearch(const std::vector<Ball>& balls, double curl) {
    MagFit top;
    for (double du = -5; du <= 5.001; du += 0.25)
        for (double dy = -5; dy <= 5.001; dy += 0.25)
            for (double dw = -5; dw <= 5.001; dw += 0.5) {
                Vec d = FromMag({du, dy, dw});
                double worst = -1e9;
                std::vector<double> gaps;
                for (auto& b : balls) {
                    double v = Into(b, d);
                    worst = (std::max)(worst, v);
                    if (strcmp(b.part, "thumb")) gaps.push_back(-v);
                }
                if (worst > -0.15) continue;
                std::partial_sort(gaps.begin(), gaps.begin() + 16, gaps.end());
                double contact = 0;
                for (int k = 0; k < 16; k++) contact += gaps[k] / 16;
                double score = contact + 0.04 * std::sqrt(du * du + dy * dy + dw * dw) + 0.15 * (1 - curl);
                if (score < top.score) top = {curl, d, score, worst, contact};
            }
    return top;
}

// Puts the magazine's grip (the left hand's) where the plugin has it (0), where the last build had it (1: wrapped
// round the Sb.5's front, too far forward on it) or where the game has it (2: the AK-74M magazine's socket, its
// fingers as made).
static const Vec kGameGripAt{0.1657, 0.0904, -11.4028};
static const Rot kGameGripRot{54.779, 180, 180};
static const Vec kLastGripAt{3.3550, -1.2405, -7.4896};
static const Rot kLastGripRot{48.354, -152.579, -145.693};
static Vec s_pluginGripAt;
static Rot s_pluginGripRot;
static bool s_pluginGripSaved = false;
static void SetMagGrip(int which) {
    Object sock = s_gripMag.GetObj("HandSocket");
    if (!s_pluginGripSaved) {
        s_pluginGripAt = sock.Get<Vec>("RelativeLocation");
        s_pluginGripRot = sock.Get<Rot>("RelativeRotation");
        s_pluginGripSaved = true;
    }
    const Vec at[] = {s_pluginGripAt, kLastGripAt, kGameGripAt};
    const Rot rot[] = {s_pluginGripRot, kLastGripRot, kGameGripRot};
    const double curl[] = {kPluginCurl, 0.8, 1.0};
    Params p(sock, "K2_SetRelativeLocationAndRotation");
    p.Set("NewLocation", at[which]).Set("NewRotation", rot[which]).Set("bSweep", false).Set("bTeleport", true).Call();
    ScaleDeltas(sock, curl[which] / s_curNow);
    s_curNow = curl[which];
}
static void ShowAkMagOn(Object mag, bool ak) {
    auto dm = Components(mag, "DynamicMeshComponent");
    for (auto [c, on] : {std::pair{mag.GetObj("RootComponent"), ak}, std::pair{dm.empty() ? Object() : dm[0], !ak}})
        if (c) {
            Params v(c, "SetVisibility");
            v.Set("bNewVisibility", on).Set("bPropagateToChildren", false).Call();
        }
}
static Vec BodyBone(const char* bone) {
    Params gl(Pawn().GetObj("Mesh"), "GetSocketLocation");
    *(uint64_t*)gl.Ptr("InSocketName") = API->MakeName(bone);
    gl.Call();
    return gl.Return<Vec>();
}
static void LogGripBody(const char* what) {
    auto balls = BodyBalls(Pawn().GetObj("Mesh"), s_gripMag);
    std::string parts;
    double worst = -1e9;
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "palm"}) {
        double w = -1e9;
        for (auto& b : balls)
            if (!strcmp(b.part, f)) w = (std::max)(w, Into(b, {0, 0, 0}));
        worst = (std::max)(worst, w);
        parts += std::format(" {} {:.0f}", f, w * 10);
    }
    Log("GRIP {}: your {} hand reaches {:.0f} mm into the Sb.5 (mm by part:{})", what, s_gripSide == "_l" ? "left" : "right", worst * 10, parts);
}
// Where the holding controller is kept: forward, down and out to the hand's side of the head, cm ([Test] GripHold).
// Too far out and the body's arm can't reach its IK target, and the hand sits off where it would in VR.
static std::string s_gripHoldCfg;
static void SetHoldPoint() {
    Object pawn = Pawn();
    Params fw(pawn, "GetActorForwardVector");
    fw.Call();
    Params rt(pawn, "GetActorRightVector");
    rt.Call();
    Vec f = fw.Return<Vec>(), r = rt.Return<Vec>(), c = CompLocation(s_gripCam);
    double fwd = 25, down = 30, out = 15;
    sscanf(s_gripHoldCfg.c_str(), "%lf,%lf,%lf", &fwd, &down, &out);
    double sgn = s_gripSide == "_l" ? -1 : 1;
    s_gripAt = {c.x + f.x * fwd + r.x * out * sgn, c.y + f.y * fwd + r.y * out * sgn, c.z - down};
}
// Does the body's hand reach its IK target? The hand bone, the IK target and the controller (world, and the hand
// off its target in the Sb.5's frame).
static void LogReach(const char* what) {
    Object pawn = Pawn();
    bool left = s_gripSide == "_l";
    Object parent = pawn.GetObj(left ? "LeftHandIKTargetParent" : "RightHandIKTargetParent");
    XfD rel = pawn.Get<XfD>(left ? "LeftHandIKTargetRelativeTransform" : "RightHandIKTargetRelativeTransform");
    if (!parent) { Log("REACH {}: no IK target parent", what); return; }
    Params tl(Lib("KismetMathLibrary"), "TransformLocation");
    XfD px = CompXf(parent);
    std::memcpy(tl.Ptr("T"), &px, sizeof px);
    tl.Set("Location", Vec{rel.t[0], rel.t[1], rel.t[2]}).Call();
    Vec ik = tl.Return<Vec>(), hand = BodyBone(left ? "hand_l" : "hand_r"), ctrl = CompLocation(s_gripCtrl);
    auto dist = [](Vec a, Vec b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
    XfD sb5 = CompXf(Components(s_gripMag, "DynamicMeshComponent")[0]);
    Vec hm = InvXfLoc(sb5, hand), im = InvXfLoc(sb5, ik);
    Log("REACH {} {}: hand bone {:.1f} cm from its IK target ({} rel ({:.2f},{:.2f},{:.2f})), {:.1f} from the controller; target {:.1f} from the controller; "
        "hand - target in the Sb.5's frame ({:.2f},{:.2f},{:.2f}); hand at ({:.2f},{:.2f},{:.2f})",
        what, left ? "left" : "right", dist(hand, ik), parent.Name(), rel.t[0], rel.t[1], rel.t[2], dist(hand, ctrl), dist(ik, ctrl), hm.x - im.x, hm.y - im.y,
        hm.z - im.z, hm.x, hm.y, hm.z);
}
// The hand and the magazine from around the hand itself, so the forearm (behind the wrist) stays out of the way:
// from the fingertips, the palm, the back of the hand, the thumb side and the little-finger side.
static void GripViews(const std::string& prefix) {
    const std::string& s = s_gripSide;
    Vec w = BodyBone(("hand" + s).c_str()), m = BodyBone(("middle_01" + s).c_str()), i1 = BodyBone(("index_01" + s).c_str()),
        p1 = BodyBone(("pinky_01" + s).c_str());
    auto norm = [](Vec v) { double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return Vec{v.x / l, v.y / l, v.z / l}; };
    Vec f = norm({m.x - w.x, m.y - w.y, m.z - w.z}), side = norm({i1.x - p1.x, i1.y - p1.y, i1.z - p1.z});
    Vec n = norm({f.y * side.z - f.z * side.y, f.z * side.x - f.x * side.z, f.x * side.y - f.y * side.x});
    Vec at{w.x + f.x * 6, w.y + f.y * 6, w.z + f.z * 6};
    Object pawn = Pawn();
    for (auto [name, dir] : std::initializer_list<std::pair<const char*, Vec>>{
             {"tips", f}, {"palmA", n}, {"palmB", {-n.x, -n.y, -n.z}}, {"thumbside", side}, {"pinkyside", {-side.x, -side.y, -side.z}}})
        CaptureWorld("grip_" + prefix + "_" + name, {pawn, s_gripMag}, {at.x + dir.x * 45, at.y + dir.y * 45, at.z + dir.z * 45}, at);
    // and from the Sb.5 itself (its frame: front u tilted 18 degrees, top w), where the hand sits along it: from the
    // palm's side, the far side and above
    XfD mx = CompXf(Components(s_gripMag, "DynamicMeshComponent")[0]);
    auto loc = [&](Vec p) {
        Params f(Lib("KismetMathLibrary"), "TransformLocation");
        std::memcpy(f.Ptr("T"), &mx, sizeof mx);
        f.Set("Location", p).Call();
        return f.Return<Vec>();
    };
    const double ct = std::cos(18 * 3.14159265358979 / 180), st = std::sin(18 * 3.14159265358979 / 180), u = 1.5, h = -9.8;
    Vec c{u * ct - h * st, 0, u * st + h * ct}, palmSide{0, s == "_l" ? -1.0 : 1.0, 0};
    for (auto [name, dir] : std::initializer_list<std::pair<const char*, Vec>>{
             {"magpalm", palmSide}, {"magfar", {0, -palmSide.y, 0}}, {"magtop", {-st, 0, ct}}})
        CaptureWorld("grip_" + prefix + "_" + name, {pawn, s_gripMag}, loc({c.x + dir.x * 40, c.y + dir.y * 40, c.z + dir.z * 40}), loc(c));
    Log("SHOT grip_{}", prefix);
}

// The seated magazine as the AK-74M's own (its mesh) or as the Sb.5 (the plugin's).
static void ShowAkMag(bool ak) {
    Object mag = s_gun.GetObj("CurrentMagazine");
    if (!mag) return;
    auto dm = Components(mag, "DynamicMeshComponent");
    for (auto [c, on] : {std::pair{mag.GetObj("RootComponent"), ak}, std::pair{dm.empty() ? Object() : dm[0], !ak}})
        if (c) {
            Params v(c, "SetVisibility");
            v.Set("bNewVisibility", on).Set("bPropagateToChildren", false).Call();
        }
}

static void Step() {
    if (s_wait > 0) { s_wait--; return; }
    switch (s_step) {
        case 0: {
            if (s_gripOnly) { s_step = 50; return; }  // [Test] GripOnly: just the hand on a spare Sb.5
            auto stations = FindAllOf("CustomizationStation_Gun_C");
            Object pawn = Pawn();
            Vec me = ActorLocation(pawn);
            double best = 1e30;
            for (auto& st : stations) {
                Vec l = ActorLocation(st);
                double d = (l.x - me.x) * (l.x - me.x) + (l.y - me.y) * (l.y - me.y);
                if (d < best && st.GetObj("WBGunBuilder")) { best = d; s_station = st; }
            }
            s_widget = s_station.GetObj("WBGunBuilder");
            for (auto& g : FindAllOf("GunAsset_C"))
                if (TextOf(g, "DisplayName") == "Saiga-12K") s_saiga = g;
            Log("{} station(s); using {}; widget {}; saiga entry {} (category {}, icon {})", stations.size(), s_station.Name(), s_widget.Name(),
                s_saiga.Name(), s_saiga.Get<uint8_t>("Category"), s_saiga.GetObj("Icon").Name());
            if (!s_widget || !s_saiga) { Error("missing pieces"); s_step = 99; return; }
            Params sl(s_station, "K2_GetActorLocation");
            sl.Call();
            Params mv(pawn, "K2_SetActorLocation");
            mv.Set("NewLocation", sl.Return<Vec>()).Set("bSweep", false).Set("bTeleport", true).Call();
            OpenShotguns();
            s_step = 10; s_wait = 60;
            return;
        }
        case 10:
            if (!Select(s_saiga)) { s_step = 99; Log("DEV TEST COMPLETE"); return; }
            s_step = 1; s_wait = 90;
            return;
        case 1: case 2: {
            Object cur = s_station.GetObj("CurrentReceiver");
            if (!cur || cur.ptr == s_before) {
                if (s_step == 1) {
                    Log("no new receiver yet - confirming");
                    Params c(s_widget, kConfirm);
                    c.Call();
                    s_step = 2; s_wait = 90;
                    return;
                }
                if (s_fallbacks++ < 2) {
                    Log("FALLBACK: the request was gated (no headset at the station) - calling the station's SpawnWeapon");
                    Params sp(s_station, "SpawnWeapon");
                    sp.SetObj("GunAsset", s_saiga).Call();
                    s_wait = 60;
                    return;
                }
                Error("spawn did not happen");
                s_step = 99;
                return;
            }
            s_gun = cur;
            Freeze(s_gun);
            Vec at = ActorLocation(Pawn());
            s_holdAt = {at.x, at.y, at.z + 120};
            s_holdRot = {0, 0, 0};
            s_holding = true;
            Log("spawned {} - holding it at ({:.0f},{:.0f},{:.0f})", s_gun.FullName(), s_holdAt.x, s_holdAt.y, s_holdAt.z);
            s_step = 3; s_wait = 240;  // dressing, then the magazine after two steady checks
            return;
        }
        case 3:
            LogGun("dressed");
            LogHandle(s_gun, "dressed");
            DumpHandle(s_gun);
            LogRoundDefaults("before firing");
            Capture("saiga");
            s_step = 35; s_wait = 30;
            return;
        // ---- the hand on the charging handle, as each of its hand sockets places it
        case 35:
            BuildHands();
            s_handShot = 0;
            s_step = 36; s_wait = 10;
            return;
        case 36:  // only this one's hand
            for (size_t i = 0; i < s_hands.size(); i++)
                if (s_hands[i]) {
                    Params v(s_hands[i], "SetVisibility");
                    v.Set("bNewVisibility", i == s_handShot).Set("bPropagateToChildren", false).Call();
                }
            s_step = 37; s_wait = 3;
            return;
        case 38: {  // the magazine hand on the AK-74M's own magazine, which the pose was made for
            std::string n = std::string("hand_") + kHands[s_handShot].name;
            CaptureFrom(n + "_ak_side", {6, 60, -9}, {6, 0, -9});
            CaptureFrom(n + "_ak_front", {55, 15, -12}, {6, 0, -9});
            CaptureFrom(n + "_ak_rear", {6, -60, -9}, {6, 0, -9});
            ShowAkMag(false);
            s_step = 39; s_wait = 2;
            return;
        }
        case 40: {  // the fitted grip on the Sb.5
            std::string n = std::string("hand_") + kHands[s_handShot].name + "_fit";
            CaptureFrom(n + "_side", {6, 60, -9}, {6, 0, -9});
            CaptureFrom(n + "_front", {55, 15, -12}, {6, 0, -9});
            CaptureFrom(n + "_under", {6, 10, -65}, {6, 0, -9});
            CaptureFrom(n + "_rear", {6, -60, -9}, {6, 0, -9});
            LogHandInMag(s_handShot);
            s_step = 39;
            return;
        }
        case 37: case 39: {
            if (s_step == 39 && !s_magFitDone && s_hands[s_handShot] && kHands[s_handShot].onMag && !kHands[s_handShot].right) {
                MagGripSearch(s_handShot);
                s_magFitDone = true;
                s_step = 40; s_wait = 3;
                return;
            }
            if (s_step == 37 && s_hands[s_handShot]) {
                LogHandInMag(s_handShot);
                std::string n = std::string("hand_") + kHands[s_handShot].name;
                if (kHands[s_handShot].onMag) {  // the magazine, from both sides, the front and below
                    CaptureFrom(n + "_side", {6, 60, -9}, {6, 0, -9});
                    CaptureFrom(n + "_front", {55, 15, -12}, {6, 0, -9});
                    CaptureFrom(n + "_under", {6, 10, -65}, {6, 0, -9});
                    CaptureFrom(n + "_rear", {6, -60, -9}, {6, 0, -9});
                    ShowAkMag(true);
                    s_step = 38; s_wait = 2;
                    return;
                } else {
                    CaptureFrom(n + "_side", {5, 70, -2}, {5, 0, -3});
                    CaptureFrom(n + "_front", {55, 30, -8}, {5, 2, -4});
                    CaptureFrom(n + "_under", {5, 22, -60}, {5, 2, -4});
                    CaptureFrom(n + "_rear", {-40, 35, 15}, {5, 2, -2});
                }
                Log("SHOT {}", n);
            }
            if (++s_handShot < s_hands.size()) { s_step = 36; return; }
            for (auto& h : s_hands)
                if (h) {
                    Params d(h, "K2_DestroyComponent");
                    d.SetObj("Object", h).Call();
                }
            s_hands.clear();
            s_step = 4; s_wait = 10;
            return;
        }
        case 4: {  // fire through the game's own FireRoundLocal: each call should spawn one slug
            s_rounds = 0;
            for (int i = 0; i < 3; i++) {
                Params fr(s_gun, "FireRoundLocal");
                fr.Call();
            }
            Log("FIRE called FireRoundLocal 3 times: {} round(s) began play", s_rounds);
            LogRoundDefaults("after firing");
            s_step = 5; s_wait = 30;
            return;
        }
        case 5: {  // the trigger in full auto, as a player would: rounds and ammo
            s_rounds = 0;
            s_gun.Set<uint8_t>("FireMode", 2);  // Auto
            s_gun.Set("CurrentTrigger", 1.0f);
            s_gun.Set("bTriggerIsEngaged", true);
            Params sf(s_gun, "SetIsFiring");
            sf.Set("Trigger", true).Call();
            Log("TRIGGER held in auto, magazine {} rounds", s_gun.GetObj("CurrentMagazine").Get<int32_t>("CurrentAmmo"));
            s_step = 6; s_wait = 120;
            return;
        }
        case 6: {
            Params sf(s_gun, "SetIsFiring");
            sf.Set("Trigger", false).Call();
            s_gun.Set("CurrentTrigger", 0.0f);
            s_gun.Set("bTriggerIsEngaged", false);
            Log("TRIGGER released: {} round(s) began play, magazine {} rounds", s_rounds, s_gun.GetObj("CurrentMagazine").Get<int32_t>("CurrentAmmo"));
            LogRoundDefaults("after auto");
            s_step = 7; s_wait = 60;
            return;
        }
        // ---- the Saiga's own action: each moving part on its own pivot, from the gun's state
        case 7: {  // on safe the game holds the handle at the safety's stop (its own CheckSafeLock)
            s_gun.Set<uint8_t>("FireMode", 0);  // Safe
            if (Object h = s_gun.GetObj("ChargingHandle")) {
                Params c(h, "CheckSafeLock");
                c.Call();
            }
            s_step = 66; s_wait = 40;
            return;
        }
        case 66: {  // pull it all the way, as a hand would, without unlocking it
            Object h = s_gun.GetObj("ChargingHandle");
            Log("SAFE handle locked {} at {:.3f}", h.GetBool("bIsLocked"), h.Get<float>("HandleLockedProgress"));
            Params p(h, "SetProgress");
            p.Set("Progress", 1.0f).Call();
            s_step = 67; s_wait = 2;
            return;
        }
        case 67:  // the receiver follows the handle
            s_gun.Set("CurrentBoltProgress", s_gun.GetObj("ChargingHandle").Get<float>("CurrentProgress"));
            s_step = 68; s_wait = 2;
            return;
        case 68: {
            LogParts("on safe, handle pulled all the way");
            LogHandle(s_gun, "on safe, handle pulled all the way");
            CaptureZoom("action_safestop");
            Params p(s_gun.GetObj("ChargingHandle"), "SetProgress");
            p.Set("Progress", 0.0f).Call();
            s_gun.Set("CurrentBoltProgress", 0.0f);
            s_step = 70; s_wait = 30;
            return;
        }
        case 70:
            LogParts("safe");
            CaptureZoom("action_safe");
            s_gun.Set<uint8_t>("FireMode", 2);  // Auto
            s_step = 71; s_wait = 40;
            return;
        case 71:
            LogParts("auto");
            CaptureZoom("action_auto");
            s_gun.Set<uint8_t>("FireMode", 1);  // Semi
            s_step = 72; s_wait = 40;
            return;
        case 72:
            LogParts("semi");
            CaptureZoom("action_semi");
            s_gun.Set<uint8_t>("FireMode", 0);  // back on safe before the trigger is pulled
            s_gun.Set("CurrentTrigger", 1.0f);
            s_step = 73; s_wait = 40;
            return;
        case 73:
            LogParts("trigger pulled");
            CaptureZoom("action_trigger");
            s_gun.Set("CurrentTrigger", 0.0f);
            s_gun.SetBool("bIsMagReleasePressed", true);
            s_step = 74; s_wait = 30;
            return;
        case 74:
            LogParts("mag release pressed");
            CaptureZoom("action_release");
            s_gun.SetBool("bIsMagReleasePressed", false);
            s_gun.Set<uint8_t>("FireMode", 1);
            s_gun.Set("CurrentBoltProgress", 0.07f);  // 0.7 cm: the bolt is turning
            s_step = 75; s_wait = 2;
            return;
        case 75:  // the charging handle pulled through its own SetProgress, as the hand pulling it does
            LogParts("carrier at 0.7 cm");
            SetHandle(0.5f);
            s_gun.Set("CurrentBoltProgress", 0.5f);
            s_step = 78; s_wait = 2;
            return;
        case 78:
            LogHandle(s_gun, "handle half back");
            SetHandle(1.0f);
            s_gun.Set("CurrentBoltProgress", 1.0f);
            s_step = 76; s_wait = 2;
            return;
        case 76:
            LogParts("carrier all the way back");
            LogHandle(s_gun, "handle all the way back");
            CaptureZoom("action_boltback");
            SetHandle(0.0f);
            s_gun.Set("CurrentBoltProgress", 0.0f);
            s_step = 77; s_wait = 30;
            return;
        case 77:
            LogParts("home");
            LogHandle(s_gun, "home");
            s_step = 9;
            return;
        case 9: {  // save it as the game saves a loadout gun, then rebuild the loadout as an operation does
            for (auto& g : FindAllOf("BP_GearManagerComponent_C"))
                if (API->GetOuter(g.ptr) == Pawn().ptr) s_gear = g;
            if (!s_gear) { Error("no gear manager"); s_step = 13; return; }
            Params sv(s_gear, "SaveGunSetup");
            sv.SetObj("GunReceiver", s_gun).Call();
            Log("LOADOUT saved {} (slot SavedGun_{}_{})", s_gun.Name(), FStr8(s_gear, "GunSetupSlotName"), FStr8(s_gear, "PrimaryGunSetupSlotName"));
            s_step = 11; s_wait = 180;  // the plugin re-saves the slot with a stable reference within a second
            return;
        }
        case 11: {
            s_holding = false;
            Params ld(s_gear, "LoadGunSetups");
            ld.Call();
            Log("LOADOUT LoadGunSetups called");
            s_step = 12; s_wait = 300;
            return;
        }
        case 12: {
            Object pg = s_gear.GetObj("PrimaryGun");
            Log("LOADOUT PrimaryGun after load: {} class {} - Saiga meshes {}, BulletData {}, magazine capacity {}", pg.Name(),
                Object((GUObject*)pg.Class()).Name(), Components(pg, "DynamicMeshComponent").size(), Object((GUObject*)pg.Get<GUClass*>("BulletData")).Name(),
                pg.GetObj("CurrentMagazine").Get<int32_t>("AmmoCapacity"));
            LogHandle(pg, "loadout gun");
            s_step = 13;
            return;
        }
        case 13:  // the Benelli M4 must NOT be on the Shotguns page any more
            s_benelli = FindObject("/Game/Weapons/Data/DA_M1014.DA_M1014");
            Log("BENELLI entry {} '{}' enabled {}", s_benelli.Name(), TextOf(s_benelli, "DisplayName"), s_benelli.GetBool("bEnabled"));
            if (!s_benelli) { s_step = 30; return; }
            OpenShotguns();
            s_step = 20; s_wait = 60;
            return;
        case 20:
            if (!Select(s_benelli)) { Log("BENELLI not in the Shotguns menu - as intended"); s_step = 30; return; }
            Error("BENELLI is still in the Shotguns menu");
            s_step = 21; s_wait = 90;
            return;
        case 21: case 22: {
            Object cur = s_station.GetObj("CurrentReceiver");
            if (!cur || cur.ptr == s_before) {
                if (s_step == 21) {
                    Params c(s_widget, kConfirm);
                    c.Call();
                    s_step = 22; s_wait = 90;
                    return;
                }
                Error("the Benelli did not spawn");
                s_step = 30;
                return;
            }
            Object mag = cur.GetObj("CurrentMagazine");
            auto* acc = (GML_TArray*)cur.PropPtr("AcceptedMagazines");
            Log("BENELLI spawned {} class {} BulletData {} magazine {} [{}] {}/{} accepts {}", cur.Name(), Object((GUObject*)cur.Class()).Name(),
                Object((GUObject*)cur.Get<GUClass*>("BulletData")).Name(), mag.Name(), Object((GUObject*)mag.Class()).Name(),
                mag.Get<int32_t>("CurrentAmmo"), mag.Get<int32_t>("AmmoCapacity"),
                acc && acc->Num ? Object(((GUObject**)acc->Data)[0]).Name() : std::string("-"));
            s_rounds = 0;
            Params fr(cur, "FireRoundLocal");
            fr.Call();
            Log("BENELLI fired once through FireRoundLocal: {} round(s) began play", s_rounds);
            s_step = 30;
            return;
        }
        case 30:
            s_step = 50;
            return;
        // ---- the player's left hand takes a spare Sb.5 from a pouch, as in VR
        case 50: {
            Object pawn = Pawn();
            Params gv(pawn, "GetVRComponents");
            gv.Call();
            s_gripCtrl = *(GUObject**)gv.Ptr("LeftController");
            s_gripCam = *(GUObject**)gv.Ptr("Camera");
            for (auto& m : FindAllOf("Magazine_AK_6L23_C")) {
                if (Components(m, "DynamicMeshComponent").empty()) continue;  // not an Sb.5
                Params pa(m, "GetAttachParentActor");
                pa.Call();
                Object parent = pa.Return<GUObject*>();
                if (!parent || parent.IsA("GunReceiver")) continue;
                Object root = parent;
                for (int g = 0; g < 8; g++) {
                    Params up(root, "GetAttachParentActor");
                    up.Call();
                    Object u = up.Return<GUObject*>();
                    if (!u) break;
                    root = u;
                }
                if (root.ptr != pawn.ptr) continue;
                s_gripMag = m;
                Log("GRIP spare Sb.5 {} in {} on {}; left controller {}", m.Name(), parent.Name(), root.Name(), s_gripCtrl.Name());
                break;
            }
            if (!s_gripCtrl || !s_gripMag) { Error("GRIP no left controller ({}) or no spare Sb.5 on the player", s_gripCtrl.Name()); s_step = 59; return; }
            // the hand rests on the magazine long enough for the game's grip highlight to pick it
            s_gripAt = ActorLocation(s_gripMag);
            s_gripHold = true;
            s_step = 51; s_wait = 90;
            return;
        }
        case 51: {
            Object pawn = Pawn();
            Vec c = CompLocation(s_gripCtrl), m = ActorLocation(s_gripMag);
            Log("GRIP left controller at ({:.0f},{:.0f},{:.0f}), magazine at ({:.0f},{:.0f},{:.0f}); highlighted: {} slot {}", c.x, c.y, c.z, m.x, m.y, m.z,
                pawn.GetObj("CurrentlyHighlightedObjectLeft").Name(), pawn.GetObj("CurrentlyHighlightingSlotLeft").Name());
            XfD def = pawn.Get<XfD>("DefaultLeftHandIKTargetOffset");
            Rot proxy = pawn.Get<Rot>("ProxyHandSocketOffsetLeft");
            Log("GRIP player: DefaultLeftHandIKTargetOffset t ({:.2f},{:.2f},{:.2f}) q ({:.3f},{:.3f},{:.3f},{:.3f}); ProxyHandSocketOffsetLeft P {:.1f} Y {:.1f} R {:.1f}",
                def.t[0], def.t[1], def.t[2], def.q[0], def.q[1], def.q[2], def.q[3], proxy.pitch, proxy.yaw, proxy.roll);
            // the curls to try, of the grip as made (the plugin has it at kPluginCurl): just the plugin's unless searching
            s_curls = s_magFitDone ? std::vector<double>{kPluginCurl} : std::vector<double>{1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4};
            if (!s_magFitDone && !s_gripCurls.empty()) {
                s_curls.clear();
                for (size_t a = 0; a < s_gripCurls.size();) {
                    size_t b = s_gripCurls.find(',', a);
                    s_curls.push_back(std::stod(s_gripCurls.substr(a, b - a)));
                    a = b == std::string::npos ? s_gripCurls.size() : b + 1;
                }
            }
            s_curlIdx = 0;
            s_curlBalls.clear();
            Object sock = s_gripMag.GetObj("HandSocket");
            ScaleDeltas(sock, s_curls[0] / s_curNow);
            s_curNow = s_curls[0];
            Log("GRIP slot grip on the magazine's hand socket: {}", SlotGrip(s_gripCtrl, s_gripMag));
            s_gripHold = false;
            s_step = 52; s_wait = 10;
            return;
        }
        case 52: {
            Params gg(s_gripCtrl, "GetGrippedObjects");
            gg.Call();
            auto* arr = (GML_TArray*)gg.Ptr("GrippedObjectsArray");
            bool ours = false;
            for (int i = 0; arr && i < arr->Num; i++) ours |= ((GUObject**)arr->Data)[i] == s_gripMag.ptr;
            Object pawn = Pawn();
            XfD ik = pawn.Get<XfD>("LeftHandIKTargetRelativeTransform");
            Log("GRIP left hand holds the Sb.5: {}; IK target on {} at ({:.2f},{:.2f},{:.2f})", ours, pawn.GetObj("LeftHandIKTargetParent").Name(), ik.t[0],
                ik.t[1], ik.t[2]);
            if (!ours) { Error("GRIP the left hand did not take the Sb.5"); s_step = 59; return; }
            // up in front of the head, as you would look at it
            Params fw(pawn, "GetActorForwardVector");
            fw.Call();
            Vec f = fw.Return<Vec>(), c = CompLocation(s_gripCam);
            SetHoldPoint();
            s_gripHold = true;
            s_step = 53; s_wait = 40;
            return;
        }
        case 53: {  // your hand on it at this curl: measured, and seen from outside, the front and below
            Object body = Pawn().GetObj("Mesh");
            auto balls = BodyBalls(body, s_gripMag);
            s_curlBalls.push_back(balls);
            std::string parts;
            double worst = -1e9;
            for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "palm"}) {
                double w = -1e9;
                for (auto& b : balls)
                    if (!strcmp(b.part, f)) w = (std::max)(w, Into(b, {0, 0, 0}));
                worst = (std::max)(worst, w);
                parts += std::format(" {} {:.0f}", f, w * 10);
            }
            Log("GRIP curl {:.2f}: your {} hand reaches {:.0f} mm into the Sb.5 (mm by part:{})", s_curNow, s_gripSide == "_l" ? "left" : "right", worst * 10, parts);
            // how bent the fingers are (degrees at the middle joint): does the grip's curl reach your hand?
            auto bend = [&](const char* f) {
                Vec a = BodyBone(std::format("{}_01{}", f, s_gripSide).c_str()), b = BodyBone(std::format("{}_02{}", f, s_gripSide).c_str()),
                    c = BodyBone(std::format("{}_03{}", f, s_gripSide).c_str());
                Vec u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - b.x, c.y - b.y, c.z - b.z};
                double d = (u.x * v.x + u.y * v.y + u.z * v.z) / std::sqrt((u.x * u.x + u.y * u.y + u.z * u.z) * (v.x * v.x + v.y * v.y + v.z * v.z));
                return std::acos(std::clamp(d, -1.0, 1.0)) * 180 / 3.14159265358979;
            };
            Log("GRIP curl {:.2f}: finger bend index {:.0f} middle {:.0f} ring {:.0f} degrees", s_curNow, bend("index"), bend("middle"), bend("ring"));
            LogReach("grip");
            {  // the hand's balls for tools\gripfit: index, part, centre (the Sb.5's frame), radius
                if (FILE* f = fopen(std::format("{}\\grip_balls{}_c{:.0f}.csv", s_shotDir, s_gripSide, s_curNow * 100).c_str(), "w")) {
                    for (size_t i = 0; i < balls.size(); i++)
                        fprintf(f, "%zu,%s,%.4f,%.4f,%.4f,%.4f\n", i, balls[i].part, balls[i].c.x, balls[i].c.y, balls[i].c.z, balls[i].r);
                    fclose(f);
                }
                if (s_curlIdx == 0) {
                    Object sock = s_gripMag.GetObj("HandSocket");
                    Object dm = Components(s_gripMag, "DynamicMeshComponent")[0];
                    Vec sl = sock.Get<Vec>("RelativeLocation"), dl = dm.Get<Vec>("RelativeLocation");
                    Rot sr = sock.Get<Rot>("RelativeRotation"), dr = dm.Get<Rot>("RelativeRotation");
                    Params gp(sock, "GetAttachParent");
                    gp.Call();
                    Log("GRIPFRAME socket on {} at ({:.4f},{:.4f},{:.4f}) rot (P {:.3f} Y {:.3f} R {:.3f}); Sb.5 mesh at ({:.4f},{:.4f},{:.4f}) rot (P {:.3f} Y {:.3f} R {:.3f}) on {}",
                        Object(gp.Return<GUObject*>()).Name(), sl.x, sl.y, sl.z, sr.pitch, sr.yaw, sr.roll, dl.x, dl.y, dl.z, dr.pitch, dr.yaw, dr.roll,
                        dm.GetObj("AttachParent").Name());
                }
            }
            if (++s_curlIdx < s_curls.size()) {  // let go, wait, then take it again at the next curl
                Params d(s_gripCtrl, "DropObjectByInterface");
                d.SetObj("ObjectToDrop", s_gripMag).Call();
                s_step = 60; s_wait = 15;
                return;
            }
            if (s_gripSide == "_l") {  // the same magazine in the right hand (its own fit), at the same curls
                Params d(s_gripCtrl, "DropObjectByInterface");
                d.SetObj("ObjectToDrop", s_gripMag).Call();
                Params gv(Pawn(), "GetVRComponents");
                gv.Call();
                s_gripCtrl = *(GUObject**)gv.Ptr("RightController");
                s_gripSide = "_r";
                SetHoldPoint();
                if (s_curls.size() > 1) { s_curlIdx = 0; s_step = 60; s_wait = 15; return; }
                bool ok = SlotGrip(s_gripCtrl, s_gripMag);
                Log("GRIP right hand slot grip: {}", ok);
                if (!ok) { s_step = 54; return; }
                s_wait = 40;
                return;
            }
            s_step = 54;
            return;
        }
        // ---- pictures, left hand: the game's grip on the AK-74M's own magazine (as it is meant to look), the same grip
        // on the Sb.5 (before), and the plugin's grip on the Sb.5 (now); cameras on the side away from the arm
        case 55: {
            Params d(s_gripCtrl, "DropObjectByInterface");
            d.SetObj("ObjectToDrop", s_gripMag).Call();
            Params gv(Pawn(), "GetVRComponents");
            gv.Call();
            s_gripCtrl = *(GUObject**)gv.Ptr("LeftController");
            s_gripSide = "_l";
            SetHoldPoint();
            SlotGrip(s_gripCtrl, s_gripMag);
            s_step = 56; s_wait = 40;
            return;
        }
        case 60: {  // the next curl, taken again after a proper release
            Object sock = s_gripMag.GetObj("HandSocket");
            ScaleDeltas(sock, s_curls[s_curlIdx] / s_curNow);
            s_curNow = s_curls[s_curlIdx];
            if (!SlotGrip(s_gripCtrl, s_gripMag)) { Error("GRIP could not take it again at curl {:.2f}", s_curNow); s_step = 59; return; }
            s_step = 53; s_wait = 40;
            return;
        }
        case 56: {  // let go; the last build's grip
            GripViews("now");
            Params d(s_gripCtrl, "DropObjectByInterface");
            d.SetObj("ObjectToDrop", s_gripMag).Call();
            SetMagGrip(1);
            s_step = 57; s_wait = 15;
            return;
        }
        case 57: SlotGrip(s_gripCtrl, s_gripMag); s_step = 58; s_wait = 40; return;
        case 58: {  // let go; the game's grip
            GripViews("last");
            LogGripBody("the last build's grip on the Sb.5");
            LogReach("last");
            Params d(s_gripCtrl, "DropObjectByInterface");
            d.SetObj("ObjectToDrop", s_gripMag).Call();
            SetMagGrip(2);
            s_step = 61; s_wait = 15;
            return;
        }
        case 61: SlotGrip(s_gripCtrl, s_gripMag); s_step = 62; s_wait = 40; return;
        case 62: {
            static int phase = 0;
            if (phase == 0) {
                GripViews("before");
                LogGripBody("the game's own grip on the Sb.5");
                LogReach("game");
                ShowAkMagOn(s_gripMag, true);
                phase = 1; s_wait = 3;
                return;
            }
            GripViews("akmag");
            ShowAkMagOn(s_gripMag, false);
            SetMagGrip(0);
            s_step = 59;
            return;
        }
        case 54: {  // per curl, the smallest move of the grip (on the magazine) that keeps your hand 1.5 mm out
            MagFit best;
            for (size_t i = 0; i < s_curlBalls.size(); i++) {
                MagFit fit = ShiftSearch(s_curlBalls[i], s_curls[i % s_curls.size()]);
                Log("GRIP {} curl {:.2f}: move ({:.2f},{:.2f},{:.2f}) on top of the plugin's - clear by {:.1f} mm, {:.1f} mm from it (score {:.3f})",
                    i < s_curls.size() ? "left" : "right", s_curls[i % s_curls.size()],
                    fit.move.x, fit.move.y, fit.move.z, -fit.worst * 10, fit.contact * 10, fit.score);
                if (fit.score < best.score) best = fit;
            }
            if (s_curlBalls.size() > 1)
                Log("GRIP BEST curl {:.2f}, move ({:.2f},{:.2f},{:.2f}) on top of the plugin's", best.curl, best.move.x, best.move.y, best.move.z);
            s_step = s_magFitDone ? 55 : 59;
            return;
        }
        case 59:
            s_gripHold = false;
            Log("DEV TEST COMPLETE");
            s_step = 99;
            return;
    }
}

GML_AWAKE() {
    s_shotDir = Config.Bind<std::string>("Test", "ShotDir", "", "Where the captures go.").Value();
    s_magFitDone = !Config.Bind("Test", "MagGripSearch", false, "Search for the magazine grip that fits the Sb.5.").Value();
    s_gripOnly = Config.Bind("Test", "GripOnly", false, "Only the left hand taking a spare Sb.5 from a pouch.").Value();
    s_gripCurls = Config.Bind<std::string>("Test", "GripCurls", "", "The finger curls MagGripSearch tries (comma separated).").Value();
    s_gripHoldCfg = Config.Bind<std::string>("Test", "GripHold", "25,30,15", "Where the hand holds the magazine: cm forward, down and out from the head (close enough that the arm reaches).").Value();
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gm) {
        std::string level = Object((GUObject*)gm).Path();
        if (level.find("/Game/Maps/MainMenu/") == 0) {
            Params p(Lib("GameplayStatics"), "OpenLevel");
            p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("LevelName", API->MakeName("/Game/Maps/TeamRoom/Teamroom")).Set("bAbsolute", true).Call();
        } else if (level.find("Teamroom") != std::string::npos) {
            s_step = 0;
            s_wait = 600;  // let the room and the player's kit load
        }
    });
    // Every round that spawns: its class and the values it was spawned with.
    HookAfter("BP_GunProjectile_C:ReceiveBeginPlay", [](Object b, GUFunction*, void*) {
        s_rounds++;
        Vec v = b.Get<Vec>("Velocity");
        Log("ROUND {} [{}] Mass {:.4f} kg Diameter {:.2f} cm speed {:.0f} m/s DamageCurve {} pooling {}", b.Name(),
            Object((GUObject*)b.Class()).Name(), b.Get<float>("Mass"), b.Get<float>("Diameter"), std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) / 100,
            b.GetObj("DamageCurve").Name(), b.GetBool("EnablePooling"));
    });
    On(GML_EVENT_TICK, [](void*) {
        if (s_step >= 0 && s_step < 99) Step();
        Hold();
        HoldGrip();
    });
    HookAfter("CustomizationStation_Gun_C:ReceiveTick", [](Object, GUFunction*, void*) { Hold(); });
    return 0;
}

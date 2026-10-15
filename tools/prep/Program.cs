// prep - turns the Saiga-12K exported from Escape from Tarkov (glTF + PNGs) into the plugin's Assets\:
//   saiga.obj         body, handguard, pistol grip, dust cover, rear sight, folding stock (static)
//   saiga_muzzle.obj  the muzzle brake (hidden while a muzzle device is mounted)
//   saiga_bolt.obj    bolt carrier with its charging handle, and the bolt     -> the AK-74M's RootBoltCarrier
//   saiga_trigger.obj / saiga_selector.obj / saiga_magrelease.obj            -> RootTrigger / RootSelector / RootMagRelease
//   saiga_mag.obj     the 5-round Sb.5 magazine, where it sits seated in the gun
//   <Part>_{Diffuse,Normal,ORM}.png for the game's M_DefaultShader (Part = Body, Bolt, Handguard, Grip,
//   Cover, Stock, Muzzle, Mag)
//
//   prep <ExportedGLTF dir> <Assets dir> <ak74m_static points.obj> [preview dir]
//
// Space: the game's (X forward, Y right, Z up, cm). In the Tarkov weapon frame -Y is forward, +Z up and +X
// the gun's left, in metres, so UE = (-y, -x, z) * 100. That is a mirror (right-handed glTF to left-handed
// UE), which with GML's no-flip OBJ import keeps faces the right way out, as GML's Blender-OBJ path does.
//
// Fit: the Saiga's pistol grip is the AK-74's plastic grip, the same part as on the game's AK-74M. A
// translation-only trimmed ICP of the Saiga's grip onto the AK-74M mesh puts the Saiga where the AK-74M's
// hands are; the offset is baked into the OBJs. The AK-74M's points come from its cooked static mesh
// (Utils\UAssetParser uexppos).
using System.Globalization;
using System.Numerics;
using System.Text;
using System.Text.Json;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
if (args.Length < 3) { Console.WriteLine("usage: prep <ExportedGLTF dir> <Assets dir> <ak74m points.obj> [preview dir]"); return 1; }
string src = args[0], outDir = args[1];
string? previewDir = args.Length > 3 ? args[3] : null;
Directory.CreateDirectory(outDir);

var weapon = Gltf.Load(Directory.GetFiles(src, "weapon_izhmash_saiga12k_10_12g_*.gltf").Single());
var magFile = Gltf.Load(Directory.GetFiles(src, "mag_saiga12_izhmash_Sb5_12x76_5_*.gltf").Single());

// texture set of each glTF material
var partOfMaterial = new Dictionary<string, string> {
    ["handguard_saiga12_izhmash_saiga12k_10_std_LOD0"] = "Handguard",
    ["muzzle_saiga12_izhmash_saiga12_std_12g_LOD0"] = "Muzzle",
    ["pistolgrip_ak_izhmash_ak74_plastic_LOD0"] = "Grip",
    ["reciever_saiga12_izhmash_saiga12k_10_std_LOD0"] = "Cover",  // Tarkov calls an AK's dust cover its receiver
    ["weapon_izhmash_saiga12k_10_12g_LOD0"] = "Body",
    ["stock_saiga12_izhmash_saiga12_fold_ak_type_LOD0"] = "Stock",
    ["weapon_izhmash_saiga12k_10_12g_bolt_LOD0"] = "Bolt",
    ["mag_saiga12_izhmash_Sb5_12x76_5_LOD0"] = "Mag",
};
// steel parts are metallic, as the game's AK-74M's are; polymer ones are not
var metalParts = new HashSet<string> { "Body", "Bolt", "Muzzle", "Cover" };
// mesh node -> OBJ file
var fileOfNode = new Dictionary<string, string> {
    ["weapon_izhmash_saiga12k_10_12g_LOD0"] = "saiga",
    ["handguard_saiga12_izhmash_saiga12k_10_std_LOD0"] = "saiga",
    ["pistolgrip_ak_izhmash_ak74_plastic_LOD0"] = "saiga",
    ["reciever_saiga12_izhmash_saiga12k_10_std_LOD0"] = "saiga",
    ["sight_rear_saiga_izhmash_saiga_fixed_LOD0"] = "saiga",
    ["stock_saiga12_izhmash_saiga12_fold_ak_type_LOD0"] = "saiga",
    ["saiga12_hammer_LOD0"] = "saiga",        // inside the receiver; the AK-74M has no hammer bone
    ["saiga_12_stock_lock_LOD0"] = "saiga",
    ["muzzle_saiga12_izhmash_saiga12_std_12g_LOD0"] = "saiga_muzzle",
    ["saiga12_bolt_LOD0"] = "saiga_bolt",
    ["saiga12_bolt_cover_LOD0"] = "saiga",  // the recoil spring assembly: stays put under the dust cover
    ["saiga12_larva_LOD0"] = "saiga_boltface",  // the bolt head: rotates to unlock, then rides the carrier
    ["saiga12_trigger_LOD0"] = "saiga_trigger",
    ["saiga12_fire_selector_LOD0"] = "saiga_selector",
    ["saiga12_release_LOD0"] = "saiga_magrelease",
};

static Vector3 ToUE(Vector3 w) => new(-w.Y * 100, -w.X * 100, w.Z * 100);
static Vector3 DirToUE(Vector3 n) => Vector3.Normalize(new Vector3(-n.Y, -n.X, n.Z));

// ---- geometry, in UE space before the fit
var meshes = new Dictionary<string, Mesh>();
Mesh Out(string file) => meshes.TryGetValue(file, out var m) ? m : meshes[file] = new Mesh();
int weaponNode = weapon.FindNode("weapon");
var wGlobal = weapon.Globals(weaponNode);
var used = new HashSet<string>();
var nodeBounds = new Dictionary<string, (Vector3 lo, Vector3 hi)>();
foreach (var (node, m) in wGlobal) {
    string name = weapon.NodeName(node);
    if (!weapon.HasMesh(node) || !fileOfNode.TryGetValue(name, out var file)) continue;
    var target = Out(file);
    int before = target.V.Count;
    weapon.AddMesh(node, m, target, partOfMaterial);
    var added = target.V.Skip(before).ToArray();
    nodeBounds[name] = (added.Aggregate(new Vector3(1e9f), Vector3.Min), added.Aggregate(new Vector3(-1e9f), Vector3.Max));
    used.Add(name);
}
foreach (var k in fileOfNode.Keys.Where(k => !used.Contains(k))) Console.WriteLine($"! mesh node {k} not found");
// The magazine: a standalone export loses the +90 degree X turn its root gets in a weapon (every part's root
// has it, and its LOD0 node the -90 that undoes it), so its LOD0 sits at the slot as the raw mesh does.
var slot = wGlobal[weapon.FindNode("mod_magazine")];
foreach (var (node, m) in magFile.Globals(0)) {
    if (!magFile.HasMesh(node) || !magFile.NodeName(node).EndsWith("_LOD0")) continue;
    magFile.AddMesh(node, m * Matrix4x4.CreateRotationX(MathF.PI / 2) * slot, Out("saiga_mag"), partOfMaterial);
}

Vector3 NodeUE(string name) => ToUE(wGlobal[weapon.FindNode(name)].Translation);

// ---- fit: the Saiga's grip onto the AK-74M's
var ak = File.ReadLines(args[2]).Where(l => l.StartsWith("v ")).Select(l => {
    var p = l.Split(' ', StringSplitOptions.RemoveEmptyEntries);
    return new Vector3(float.Parse(p[1]), float.Parse(p[2]), float.Parse(p[3]));
}).ToArray();
var grip = meshes["saiga"].PositionsOf("Grip");
// start: the chambers on top of each other (the AK-74M's Round Chamber Location, actor space)
var akChamber = new Vector3(4.366f + 2.718f, 0, 0.068f);
var offset = akChamber - NodeUE("patron_in_weapon");
Console.WriteLine($"start offset (chambers together): {V(offset)}");
for (int it = 0; it < 100; it++) {
    var lo = grip.Aggregate(new Vector3(1e9f), (a, p) => Vector3.Min(a, p + offset)) - new Vector3(3);
    var hi = grip.Aggregate(new Vector3(-1e9f), (a, p) => Vector3.Max(a, p + offset)) + new Vector3(3);
    var target = ak.Where(p => p.X >= lo.X && p.Y >= lo.Y && p.Z >= lo.Z && p.X <= hi.X && p.Y <= hi.Y && p.Z <= hi.Z).ToArray();
    var d = grip.Select(p => { var q = p + offset; var t = target.MinBy(t => Vector3.DistanceSquared(t, q)); return t - q; })
                .OrderBy(v => v.LengthSquared()).Take(grip.Length * 7 / 10).ToArray();
    var step = new Vector3(Median(d.Select(v => v.X)), Median(d.Select(v => v.Y)), Median(d.Select(v => v.Z)));
    offset += step;
    if (step.Length() < 1e-4f || it == 99) {
        float rms = MathF.Sqrt(d.Average(v => v.LengthSquared()));
        Console.WriteLine($"fit after {it + 1} steps: offset {V(offset)}, rms {rms:F3} cm over {d.Length} of {grip.Length} grip vertices ({target.Length} AK-74M points)");
        break;
    }
}
foreach (var m in meshes.Values) m.Move(offset);

// ---- where the Saiga's parts are (actor space, cm), next to the AK-74M's anchors
Console.WriteLine("\nSaiga points (actor space, cm, fitted):");
foreach (var n in new[] { "fireport", "mod_muzzle", "patron_in_weapon", "shellport", "smokeport", "mod_magazine", "mod_magazine_new",
                          "weapon_trigger", "weapon_selector", "weapon_release", "weapon_bolt", "mod_charge", "weapon_hammer",
                          "mod_pistol_grip", "mod_handguard", "mod_reciever", "mod_sight_rear", "mod_mount_000", "mod_mount_001",
                          "mod_stock", "weapon_stock_lock", "mod_align_front", "weapon_L_hand_marker", "weapon_R_hand_marker" })
    Console.WriteLine($"  {n,-22} {V(NodeUE(n) + offset)}");
foreach (var (file, m) in meshes.OrderBy(k => k.Key)) {
    var (lo, hi) = m.Bounds();
    Console.WriteLine($"  {file + ".obj",-22} X[{lo.X:F2}..{hi.X:F2}] Y[{lo.Y:F2}..{hi.Y:F2}] Z[{lo.Z:F2}..{hi.Z:F2}]  {m.V.Count} verts, {m.TriCount} tris");
}
var stockLo = meshes["saiga"].PositionsOf("Stock").Min(p => p.X);
{
    var butt = meshes["saiga"].PositionsOf("Stock").Where(p => p.X < stockLo + 1).ToArray();
    Console.WriteLine($"  stock butt at X {stockLo:F2}, butt plate Z[{butt.Min(p => p.Z):F2}..{butt.Max(p => p.Z):F2}] Y[{butt.Min(p => p.Y):F2}..{butt.Max(p => p.Y):F2}]");
}
foreach (var part in new[] { "Handguard" }) {
    var hp = meshes["saiga"].PositionsOf(part);
    Console.WriteLine($"  {part}: X[{hp.Min(p => p.X):F2}..{hp.Max(p => p.X):F2}] Y[{hp.Min(p => p.Y):F2}..{hp.Max(p => p.Y):F2}] Z[{hp.Min(p => p.Z):F2}..{hp.Max(p => p.Z):F2}]");
}

Console.WriteLine("\nmoving parts (fitted bounds, cm):");
foreach (var (name, (lo, hi)) in nodeBounds.OrderBy(k => k.Key))
    Console.WriteLine($"  {name,-50} X[{lo.X + offset.X:F2}..{hi.X + offset.X:F2}] Y[{lo.Y + offset.Y:F2}..{hi.Y + offset.Y:F2}] Z[{lo.Z + offset.Z:F2}..{hi.Z + offset.Z:F2}]");
{
    var knob = meshes["saiga_bolt"].V.Where(v => v.Y > 3.0f).ToArray();
    if (knob.Length > 0) {
        var lo = knob.Aggregate(new Vector3(1e9f), Vector3.Min); var hi = knob.Aggregate(new Vector3(-1e9f), Vector3.Max);
        Console.WriteLine($"  charging handle knob (bolt verts with Y > 3): X[{lo.X:F2}..{hi.X:F2}] Y[{lo.Y:F2}..{hi.Y:F2}] Z[{lo.Z:F2}..{hi.Z:F2}] centre {V((lo + hi) / 2)}");
    }
}

// ---- OBJs
foreach (var (file, m) in meshes) File.WriteAllText(Path.Combine(outDir, file + ".obj"), m.ToObj(file));

// ---- textures
var done = new HashSet<string>();
foreach (var g in new[] { weapon, magFile })
    foreach (var mat in g.Materials()) {
        if (!partOfMaterial.TryGetValue(mat.Name, out var part) || !done.Add(part)) continue;
        Textures.Write(g.Dir, mat, part, metalParts.Contains(part), outDir);
        Console.WriteLine($"textures: {part} <- {mat.Name}");
    }

Render.Icon(Path.Combine(outDir, "Icon.png"), meshes.Where(m => m.Key != "saiga_mag").Select(m => m.Value), outDir);
if (previewDir != null) ActionStudy.Run(previewDir, meshes, outDir, NodeUE("weapon_selector") + offset);
if (previewDir != null) {
    Preview.Draw(Path.Combine(previewDir, "fit.png"), ak, meshes);
    Render.Views(previewDir, meshes, outDir, ak);
}
Console.WriteLine("DONE");
return 0;

static float Median(IEnumerable<float> xs) { var a = xs.OrderBy(x => x).ToArray(); return a[a.Length / 2]; }
static string V(Vector3 v) => $"({v.X:F3}, {v.Y:F3}, {v.Z:F3})";

class Mesh {
    public List<Vector3> V = new(), N = new();
    public List<Vector2> T = new();
    public List<(string group, int a, int b, int c)> Tris = new();
    public int TriCount => Tris.Count;
    public Vector3[] PositionsOf(string group) => Tris.Where(t => t.group == group).SelectMany(t => new[] { t.a, t.b, t.c }).Distinct().Select(i => V[i]).ToArray();
    public void Move(Vector3 d) { for (int i = 0; i < V.Count; i++) V[i] += d; }
    public (Vector3, Vector3) Bounds() => (V.Aggregate(new Vector3(1e9f), Vector3.Min), V.Aggregate(new Vector3(-1e9f), Vector3.Max));
    public string ToObj(string name) {
        var sb = new StringBuilder($"# {name}: Saiga-12K (Escape from Tarkov), Unreal space, cm\n");
        foreach (var v in V) sb.Append($"v {v.X:0.#####} {v.Y:0.#####} {v.Z:0.#####}\n");
        foreach (var t in T) sb.Append($"vt {t.X:0.######} {1 - t.Y:0.######}\n");  // OBJ V runs bottom-up, glTF's top-down
        foreach (var n in N) sb.Append($"vn {n.X:0.#####} {n.Y:0.#####} {n.Z:0.#####}\n");
        foreach (var grp in Tris.GroupBy(t => t.group)) {
            sb.Append($"usemtl {grp.Key}\n");
            foreach (var (_, a, b, c) in grp) sb.Append($"f {a + 1}/{a + 1}/{a + 1} {b + 1}/{b + 1}/{b + 1} {c + 1}/{c + 1}/{c + 1}\n");
        }
        return sb.ToString();
    }
}

record Material(string Name, string? BaseColor, Vector4 BaseColorFactor, string? MetalRough, string? Normal);

class Gltf {
    public JsonElement J;
    public string Dir = "";
    byte[][] buffers = [];

    public static Gltf Load(string path) {
        var g = new Gltf { J = JsonDocument.Parse(File.ReadAllText(path)).RootElement, Dir = Path.GetDirectoryName(path)! };
        g.buffers = g.J.GetProperty("buffers").EnumerateArray().Select(b => File.ReadAllBytes(Path.Combine(g.Dir, Uri.UnescapeDataString(b.GetProperty("uri").GetString()!)))).ToArray();
        return g;
    }
    JsonElement Node(int i) => J.GetProperty("nodes")[i];
    public string NodeName(int i) => Node(i).TryGetProperty("name", out var n) ? n.GetString()! : "";
    public bool HasMesh(int i) => Node(i).TryGetProperty("mesh", out _);
    public int FindNode(string name) {
        for (int i = 0; i < J.GetProperty("nodes").GetArrayLength(); i++) if (NodeName(i) == name) return i;
        throw new Exception($"node {name} not found");
    }
    Matrix4x4 Local(int i) {
        var n = Node(i);
        var t = n.TryGetProperty("translation", out var tt) ? new Vector3(F(tt[0]), F(tt[1]), F(tt[2])) : Vector3.Zero;
        var r = n.TryGetProperty("rotation", out var rr) ? new Quaternion(F(rr[0]), F(rr[1]), F(rr[2]), F(rr[3])) : Quaternion.Identity;
        var s = n.TryGetProperty("scale", out var ss) ? new Vector3(F(ss[0]), F(ss[1]), F(ss[2])) : Vector3.One;
        return Matrix4x4.CreateScale(s) * Matrix4x4.CreateFromQuaternion(r) * Matrix4x4.CreateTranslation(t);
    }
    static float F(JsonElement e) => e.GetSingle();
    // Every node under `root`, in root's frame (root's own transform left out).
    public Dictionary<int, Matrix4x4> Globals(int root) {
        var d = new Dictionary<int, Matrix4x4>();
        void Walk(int i, Matrix4x4 parent) {
            if (!Node(i).TryGetProperty("children", out var ch)) return;
            foreach (var c in ch.EnumerateArray()) { int k = c.GetInt32(); var m = Local(k) * parent; d[k] = m; Walk(k, m); }
        }
        d[root] = Matrix4x4.Identity;
        Walk(root, Matrix4x4.Identity);
        return d;
    }
    float[] Read(int accessor, out int comps) {
        var a = J.GetProperty("accessors")[accessor];
        var bv = J.GetProperty("bufferViews")[a.GetProperty("bufferView").GetInt32()];
        var buf = buffers[bv.TryGetProperty("buffer", out var b) ? b.GetInt32() : 0];
        int count = a.GetProperty("count").GetInt32(), ct = a.GetProperty("componentType").GetInt32();
        comps = a.GetProperty("type").GetString() switch { "SCALAR" => 1, "VEC2" => 2, "VEC3" => 3, "VEC4" => 4, var x => throw new Exception(x) };
        int size = ct switch { 5126 or 5125 => 4, 5123 => 2, 5121 => 1, _ => throw new Exception($"componentType {ct}") };
        int start = (bv.TryGetProperty("byteOffset", out var o) ? o.GetInt32() : 0) + (a.TryGetProperty("byteOffset", out var ao) ? ao.GetInt32() : 0);
        int stride = bv.TryGetProperty("byteStride", out var st) ? st.GetInt32() : size * comps;
        var r = new float[count * comps];
        for (int i = 0; i < count; i++)
            for (int c = 0; c < comps; c++) {
                int at = start + i * stride + c * size;
                r[i * comps + c] = ct switch { 5126 => BitConverter.ToSingle(buf, at), 5125 => BitConverter.ToUInt32(buf, at), 5123 => BitConverter.ToUInt16(buf, at), _ => buf[at] };
            }
        return r;
    }
    public IEnumerable<Material> Materials() {
        var images = J.GetProperty("images").EnumerateArray().Select(i => i.GetProperty("uri").GetString()!).ToArray();
        var textures = J.GetProperty("textures").EnumerateArray().Select(t => images[t.GetProperty("source").GetInt32()]).ToArray();
        string? Tex(JsonElement e, string key) => e.TryGetProperty(key, out var t) ? textures[t.GetProperty("index").GetInt32()] : null;
        foreach (var m in J.GetProperty("materials").EnumerateArray()) {
            var pbr = m.GetProperty("pbrMetallicRoughness");
            var f = pbr.TryGetProperty("baseColorFactor", out var bf) ? new Vector4(F(bf[0]), F(bf[1]), F(bf[2]), F(bf[3])) : Vector4.One;
            yield return new Material(m.GetProperty("name").GetString()!, Tex(pbr, "baseColorTexture"), f, Tex(pbr, "metallicRoughnessTexture"), Tex(m, "normalTexture"));
        }
    }
    public void AddMesh(int node, Matrix4x4 world, Mesh into, Dictionary<string, string> partOfMaterial) {
        var mats = J.GetProperty("materials");
        foreach (var prim in J.GetProperty("meshes")[Node(node).GetProperty("mesh").GetInt32()].GetProperty("primitives").EnumerateArray()) {
            var at = prim.GetProperty("attributes");
            var pos = Read(at.GetProperty("POSITION").GetInt32(), out _);
            var nrm = Read(at.GetProperty("NORMAL").GetInt32(), out _);
            var uv = Read(at.GetProperty("TEXCOORD_0").GetInt32(), out _);
            var idx = Read(prim.GetProperty("indices").GetInt32(), out _);
            string group = partOfMaterial[mats[prim.GetProperty("material").GetInt32()].GetProperty("name").GetString()!];
            int baseIndex = into.V.Count;
            for (int i = 0; i < pos.Length / 3; i++) {
                into.V.Add(ToUE(Vector3.Transform(new Vector3(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]), world)));
                into.N.Add(DirToUE(Vector3.TransformNormal(new Vector3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]), world)));
                into.T.Add(new Vector2(uv[i * 2], uv[i * 2 + 1]));
            }
            for (int i = 0; i < idx.Length; i += 3)
                into.Tris.Add((group, baseIndex + (int)idx[i], baseIndex + (int)idx[i + 1], baseIndex + (int)idx[i + 2]));
        }
    }
    static Vector3 ToUE(Vector3 w) => new(-w.Y * 100, -w.X * 100, w.Z * 100);
    static Vector3 DirToUE(Vector3 n) => Vector3.Normalize(new Vector3(-n.Y, -n.X, n.Z));
}

static class Textures {
    static float ToLinear(float c) => c <= 0.04045f ? c / 12.92f : MathF.Pow((c + 0.055f) / 1.055f, 2.4f);
    static float ToSrgb(float c) => c <= 0.0031308f ? c * 12.92f : 1.055f * MathF.Pow(c, 1 / 2.4f) - 0.055f;
    static byte B(float x) => (byte)Math.Clamp((int)MathF.Round(x * 255), 0, 255);

    public static void Write(string dir, Material m, string part, bool metal, string outDir) {
        // Diffuse: the albedo times the material's base colour factor (glTF: linear), as Tarkov's shader tints it
        using (var img = Image.Load<Rgba32>(Path.Combine(dir, m.BaseColor!))) {
            img.ProcessPixelRows(rows => {
                for (int y = 0; y < rows.Height; y++) {
                    var r = rows.GetRowSpan(y);
                    for (int x = 0; x < r.Length; x++) {
                        var p = r[x];
                        r[x] = new Rgba32(B(ToSrgb(ToLinear(p.R / 255f) * m.BaseColorFactor.X)), B(ToSrgb(ToLinear(p.G / 255f) * m.BaseColorFactor.Y)),
                                          B(ToSrgb(ToLinear(p.B / 255f) * m.BaseColorFactor.Z)), 255);
                    }
                }
            });
            img.SaveAsPng(Path.Combine(outDir, part + "_Diffuse.png"));
        }
        // Normal: Unity's (OpenGL, green up) to Unreal's (DirectX, green down)
        using (var img = Image.Load<Rgba32>(Path.Combine(dir, m.Normal!))) {
            img.ProcessPixelRows(rows => {
                for (int y = 0; y < rows.Height; y++) {
                    var r = rows.GetRowSpan(y);
                    for (int x = 0; x < r.Length; x++) r[x] = new Rgba32(r[x].R, (byte)(255 - r[x].G), r[x].B, 255);
                }
            });
            img.SaveAsPng(Path.Combine(outDir, part + "_Normal.png"));
        }
        // ORM: no occlusion map (white), glTF roughness (G), metallic by part
        using (var img = Image.Load<Rgba32>(Path.Combine(dir, m.MetalRough!))) {
            byte metallic = metal ? (byte)255 : (byte)0;
            img.ProcessPixelRows(rows => {
                for (int y = 0; y < rows.Height; y++) {
                    var r = rows.GetRowSpan(y);
                    for (int x = 0; x < r.Length; x++) r[x] = new Rgba32(255, r[x].G, metallic, 255);
                }
            });
            img.SaveAsPng(Path.Combine(outDir, part + "_ORM.png"));
        }
    }
}

static class Preview {
    // Side view (X right, Z up) over top view (X right, +Y down): the AK-74M's points grey, the Saiga by part.
    public static void Draw(string path, Vector3[] ak, Dictionary<string, Mesh> meshes) {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        const float s = 14, x0 = -55, x1 = 75, z0 = -25, z1 = 12, y0 = -8, y1 = 8;
        int w = (int)((x1 - x0) * s), hSide = (int)((z1 - z0) * s), hTop = (int)((y1 - y0) * s);
        using var img = new Image<Rgba32>(w, hSide + hTop + 10, new Rgba32(255, 255, 255, 255));
        void Dot(Vector3 p, Rgba32 c) {
            int x = (int)((p.X - x0) * s), y = (int)((z1 - p.Z) * s);
            if (x >= 0 && x < w && y >= 0 && y < hSide) img[x, y] = c;
            int yt = hSide + 10 + (int)((p.Y - y0) * s);
            if (x >= 0 && x < w && yt >= hSide + 10 && yt < img.Height) img[x, yt] = c;
        }
        foreach (var p in ak) Dot(p, new Rgba32(150, 150, 150, 255));
        var colors = new Dictionary<string, Rgba32> {
            ["Body"] = new(200, 0, 0, 255), ["Bolt"] = new(255, 120, 0, 255), ["Handguard"] = new(0, 150, 0, 255), ["Grip"] = new(0, 0, 255, 255),
            ["Cover"] = new(160, 0, 160, 255), ["Stock"] = new(0, 160, 160, 255), ["Muzzle"] = new(120, 80, 0, 255), ["Mag"] = new(220, 0, 220, 255),
        };
        foreach (var m in meshes.Values)
            foreach (var t in m.Tris) foreach (var i in new[] { t.a, t.b, t.c }) Dot(m.V[i], colors[t.group]);
        // the AK-74M's anchors (actor space): black crosses
        Vector3[] anchors = [new(47.64f, 0, 0.068f), new(22.24f, 0, 0.27f), new(-9.98f, 0, -7.27f), new(2.82f, 0, -4.49f), new(7.08f, 0, 0.068f),
                             new(8.57f, 4.32f, 1.21f), new(-42.17f, 0, -7.07f), new(-4.25f, -2.58f, -0.97f), new(-1.168f, 0, 0.006f),
                             new(-8.397f, 0, -0.704f), new(-5.972f, 0, -3.223f), new(-1.108f, -0.796f, -5.208f)];
        foreach (var a in anchors)
            for (float d = -0.6f; d <= 0.6f; d += 0.05f) { Dot(a + new Vector3(d, 0, 0), new(0, 0, 0, 255)); Dot(a + new Vector3(0, d, d), new(0, 0, 0, 255)); }
        img.SaveAsPng(path);
    }
}

// A textured, lit render of the converted meshes (orthographic: right side, left side, top), each with the
// AK-74M's points over it in blue: checks the fit, the faces' winding and the UVs before the game sees them.
// Also the spawner icon, drawn like the game's own thumbnails (right side, transparent, 512 x 512).
static class Render {
    static Dictionary<string, Image<Rgba32>> Textures(IEnumerable<Mesh> meshes, string assets) =>
        meshes.SelectMany(m => m.Tris.Select(t => t.group)).Distinct().ToDictionary(p => p, p => Image.Load<Rgba32>(Path.Combine(assets, p + "_Diffuse.png")));

    // view f: point -> (screen x, screen y down, depth toward the viewer), in cm; s pixels per cm
    static float[] Raster(Image<Rgba32> img, IEnumerable<Mesh> meshes, Dictionary<string, Image<Rgba32>> tex, Func<Vector3, Vector3> f,
                          Vector3 eye, float x0, float y0, float s, bool markBack) {
        int w = img.Width, h = img.Height;
        var depth = new float[w * h];
        Array.Fill(depth, float.NegativeInfinity);
        var light = Vector3.Normalize(eye + new Vector3(0.3f, 0, 0.6f));
        foreach (var m in meshes)
            foreach (var (part, a, b, c) in m.Tris) {
                var pa = f(m.V[a]); var pb = f(m.V[b]); var pc = f(m.V[c]);
                Vector2 A = new((pa.X - x0) * s, (pa.Y - y0) * s), B = new((pb.X - x0) * s, (pb.Y - y0) * s), C = new((pc.X - x0) * s, (pc.Y - y0) * s);
                float area = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
                if (MathF.Abs(area) < 1e-6f) continue;
                int minX = Math.Max(0, (int)MathF.Floor(MathF.Min(A.X, MathF.Min(B.X, C.X)))), maxX = Math.Min(w - 1, (int)MathF.Ceiling(MathF.Max(A.X, MathF.Max(B.X, C.X))));
                int minY = Math.Max(0, (int)MathF.Floor(MathF.Min(A.Y, MathF.Min(B.Y, C.Y)))), maxY = Math.Min(h - 1, (int)MathF.Ceiling(MathF.Max(A.Y, MathF.Max(B.Y, C.Y))));
                var t = tex[part];
                for (int y = minY; y <= maxY; y++)
                    for (int x = minX; x <= maxX; x++) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float wa = ((B.X - px) * (C.Y - py) - (B.Y - py) * (C.X - px)) / area;
                        float wb = ((C.X - px) * (A.Y - py) - (C.Y - py) * (A.X - px)) / area;
                        float wc = 1 - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0) continue;
                        float z = wa * pa.Z + wb * pb.Z + wc * pc.Z;
                        if (z <= depth[y * w + x]) continue;
                        depth[y * w + x] = z;
                        var uv = wa * m.T[a] + wb * m.T[b] + wc * m.T[c];
                        var n = Vector3.Normalize(wa * m.N[a] + wb * m.N[b] + wc * m.N[c]);
                        // a face whose normal points away from the viewer is drawn red: wrong winding or normals
                        bool back = Vector3.Dot(n, eye) < -0.2f;
                        int tx = (int)((uv.X - MathF.Floor(uv.X)) * t.Width) % t.Width, ty = (int)((uv.Y - MathF.Floor(uv.Y)) * t.Height) % t.Height;
                        var c0 = t[tx, ty];
                        float lit = 0.35f + 1.6f * MathF.Max(0, Vector3.Dot(n, back ? -n : light));
                        img[x, y] = back && markBack ? new Rgba32(255, 0, 0, 255)
                            : new Rgba32((byte)Math.Min(255, c0.R * lit), (byte)Math.Min(255, c0.G * lit), (byte)Math.Min(255, c0.B * lit), 255);
                    }
            }
        return depth;
    }

    public static void Views(string dir, Dictionary<string, Mesh> meshes, string assets, Vector3[] ak) {
        var tex = Textures(meshes.Values, assets);
        (string name, Func<Vector3, Vector3> f, Vector3 eye)[] views = [
            ("right", p => new(p.X, -p.Z, p.Y), new(0, 1, 0)),     // seen from +Y (the gun's right)
            ("left", p => new(-p.X, -p.Z, -p.Y), new(0, -1, 0)),
            ("top", p => new(p.X, p.Y, p.Z), new(0, 0, 1)),
        ];
        const float s = 16;
        foreach (var (name, f, eye) in views) {
            float x0 = -45, x1 = 56, y0 = -6, y1 = 20;
            if (name == "left") { x0 = -56; x1 = 45; }
            if (name == "top") { y0 = -7; y1 = 7; }
            using var img = new Image<Rgba32>((int)((x1 - x0) * s), (int)((y1 - y0) * s), new Rgba32(235, 235, 235, 255));
            var depth = Raster(img, meshes.Values, tex, f, eye, x0, y0, s, true);
            foreach (var p in ak) {
                var q = f(p);
                int x = (int)((q.X - x0) * s), y = (int)((q.Y - y0) * s);
                if (x >= 0 && x < img.Width && y >= 0 && y < img.Height && q.Z >= depth[y * img.Width + x] - 0.3f) img[x, y] = new Rgba32(0, 200, 255, 255);
            }
            img.SaveAsPng(Path.Combine(dir, $"render_{name}.png"));
        }
        foreach (var t in tex.Values) t.Dispose();
    }

    // The right side of the region X[x0..x1] Z[z0..z1] (cm), s pixels per cm.
    public static void Region(string path, Dictionary<string, Mesh> meshes, string assets, float x0, float x1, float z0, float z1, float s) {
        var tex = Textures(meshes.Values, assets);
        using var img = new Image<Rgba32>((int)((x1 - x0) * s), (int)((z1 - z0) * s), new Rgba32(235, 235, 235, 255));
        Raster(img, meshes.Values, tex, p => new(p.X, -p.Z, p.Y), new(0, 1, 0), x0, -z1, s, true);
        img.SaveAsPng(path);
        foreach (var t in tex.Values) t.Dispose();
    }

    public static void Icon(string path, IEnumerable<Mesh> meshes, string assets) {
        var list = meshes.ToList();
        var tex = Textures(list, assets);
        var lo = list.Select(m => m.Bounds().Item1).Aggregate(Vector3.Min);
        var hi = list.Select(m => m.Bounds().Item2).Aggregate(Vector3.Max);
        // the game's thumbnails: the gun across ~86% of the width, centred, 4x supersampled
        const int size = 512, ss = 4;
        float s = size * 0.86f / (hi.X - lo.X) * ss;
        float cx = (lo.X + hi.X) / 2, cz = (lo.Z + hi.Z) / 2;
        float half = size * ss / 2f / s;
        using var big = new Image<Rgba32>(size * ss, size * ss, new Rgba32(0, 0, 0, 0));
        Raster(big, list, tex, p => new(p.X, -p.Z, p.Y), new(0, 1, 0), cx - half, -cz - half, s, false);
        using var icon = new Image<Rgba32>(size, size);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                float r = 0, g = 0, b = 0, a = 0;
                for (int j = 0; j < ss; j++)
                    for (int i = 0; i < ss; i++) {
                        var p = big[x * ss + i, y * ss + j];
                        float pa = p.A / 255f;
                        r += p.R * pa; g += p.G * pa; b += p.B * pa; a += pa;
                    }
                icon[x, y] = a > 0 ? new Rgba32((byte)(r / a), (byte)(g / a), (byte)(b / a), (byte)(a / (ss * ss) * 255)) : new Rgba32(0, 0, 0, 0);
            }
        icon.SaveAsPng(path);
        foreach (var t in tex.Values) t.Dispose();
    }
}

// The action from the right, close up, for picking the Saiga's own numbers (selector angles: positive = lever
// tip down, as the plugin poses it):
//   travel_<cm>.png   the carrier (with its charging handle) and the bolt pulled back that far, dust cover off:
//                     the carrier's rear against the receiver's, the bolt face behind the magazine
//   safe_<deg>.png    the lever turned up that far, the charging handle pulled back to where the lever stops it
// and prints the carrier's room behind it, and for each lever angle where the handle's stem meets the lever.
static class ActionStudy {
    static Mesh Map(Mesh m, Func<Vector3, Vector3> p, Func<Vector3, Vector3> n, string? drop = null) {
        var r = new Mesh { T = m.T, Tris = drop == null ? m.Tris : m.Tris.Where(t => t.group != drop).ToList() };
        foreach (var v in m.V) r.V.Add(p(v));
        foreach (var v in m.N) r.N.Add(n(v));
        return r;
    }
    static Func<Vector3, Vector3> Turn(Vector3 pivot, float deg) {
        float r = deg * MathF.PI / 180, c = MathF.Cos(r), s = MathF.Sin(r);
        return v => { var d = v - pivot; return pivot + new Vector3(d.X * c + d.Z * s, d.Y, -d.X * s + d.Z * c); };
    }
    static Func<Vector3, Vector3> TurnDir(float deg) {
        float r = deg * MathF.PI / 180, c = MathF.Cos(r), s = MathF.Sin(r);
        return n => new Vector3(n.X * c + n.Z * s, n.Y, -n.X * s + n.Z * c);
    }
    // points spread over a mesh's triangles (whose corners all pass `keep`)
    static IEnumerable<Vector3> Samples(Mesh m, Func<Vector3, bool> keep, int k = 24) {
        foreach (var (_, a, b, c) in m.Tris) {
            if (!keep(m.V[a]) || !keep(m.V[b]) || !keep(m.V[c])) continue;
            for (int i = 0; i <= k; i++)
                for (int j = 0; i + j <= k; j++) {
                    float u = i / (float)k, w = j / (float)k;
                    yield return m.V[a] + u * (m.V[b] - m.V[a]) + w * (m.V[c] - m.V[a]);
                }
        }
    }

    public static void Run(string dir, Dictionary<string, Mesh> meshes, string assets, Vector3 selectorPivot) {
        Func<Vector3, Vector3> same = v => v;
        var carrier = meshes["saiga_bolt"];
        // the carrier's room: its tail stops against the front of the recoil spring guide's rear block (the spring
        // assembly, within the tail's height)
        float carrierRear = carrier.V.Min(v => v.X);
        var tail = carrier.V.Where(v => v.X < carrierRear + 0.5f).ToArray();
        float tailLo = tail.Min(v => v.Z), tailHi = tail.Max(v => v.Z);
        float guideFace = meshes["saiga"].PositionsOf("Bolt").Where(v => v.X < carrierRear - 2 && v.Z > tailLo && v.Z < tailHi)
                                         .Select(v => v.X).DefaultIfEmpty(float.NaN).Max();
        float fullTravel = carrierRear - guideFace - 0.1f;  // a millimetre short of touching
        float boltFace = meshes["saiga_boltface"].V.Max(v => v.X);
        var magLo = meshes["saiga_mag"].V.Aggregate(new Vector3(1e9f), Vector3.Min);
        Console.WriteLine($"\naction: carrier tail X {carrierRear:F2} (Z {tailLo:F2}..{tailHi:F2}), spring guide block face X {guideFace:F2} -> full travel {fullTravel:F2} cm; " +
                          $"bolt face X {boltFace:F2}, magazine rear X {magLo.X:F2}");
        foreach (float t in new[] { 0f, 10, 11, MathF.Round(fullTravel, 1), 13 }) {
            var set = meshes.Where(m => m.Key is not ("saiga_bolt" or "saiga_boltface")).ToDictionary(m => m.Key, m => m.Value);
            set["saiga"] = Map(meshes["saiga"], same, same, "Cover");
            set["saiga_bolt"] = Map(carrier, v => v - new Vector3(t, 0, 0), same);
            float lag = MathF.Max(0, t - 0.9f);
            set["saiga_boltface"] = Map(meshes["saiga_boltface"], v => v - new Vector3(lag, 0, 0), same);
            Render.Region(Path.Combine(dir, $"travel_{t:0.#}.png"), set, assets, -17, 14, -9, 5, 40);
            Console.WriteLine($"  travel {t,4:0.#}: carrier rear X {carrierRear - t:F2}, bolt face X {boltFace - lag:F2}");
        }
        // the handle's stem, where it comes out of the receiver (carrier outside the right wall, before the knob)
        var stem = carrier.V.Where(v => v.Y > 1.9f && v.Y < 2.6f).ToArray();
        float stemRear = stem.Min(v => v.X), stemLo = stem.Min(v => v.Z), stemHi = stem.Max(v => v.Z);
        Console.WriteLine($"  handle stem: rear X {stemRear:F2}, Z[{stemLo:F2}..{stemHi:F2}]");
        var sel = meshes["saiga_selector"];
        foreach (float a in new[] { -4f, -6, -8, -10, -12, -14, -16, -18, -20, -22, -24 }) {
            var lever = Map(sel, Turn(selectorPivot, a), TurnDir(a));
            // the lever where the stem passes: outside the receiver wall, within the stem's height
            var hit = Samples(lever, v => v.Y > 1.75f).Where(v => v.Z > stemLo + 0.05f && v.Z < stemHi - 0.05f).ToArray();
            float front = hit.Length > 0 ? hit.Max(v => v.X) : float.NaN;
            float stop = hit.Length > 0 ? stemRear - front : float.NaN;
            float top = Samples(lever, v => v.Y > 1.75f).Where(v => v.X > -6).Max(v => v.Z);
            Console.WriteLine($"  lever {a,4:0}: top edge (forward of X -6) Z {top:F2}; " +
                              (hit.Length == 0 ? "clear of the stem's path"
                               : stop >= fullTravel ? $"touches the stem's path at X {front:F2}, behind the full stroke - clear"
                               : $"in the stem's path, front X {front:F2} -> handle stops after {stop:F2} cm ({stop / fullTravel:F3} of the travel)"));
            var set = meshes.Where(m => m.Key is not ("saiga_bolt" or "saiga_selector")).ToDictionary(m => m.Key, m => m.Value);
            set["saiga_selector"] = lever;
            float back = float.IsNaN(stop) ? 0 : MathF.Max(0, stop);
            set["saiga_bolt"] = Map(carrier, v => v - new Vector3(back, 0, 0), same);
            Render.Region(Path.Combine(dir, $"safe_{a:0}.png"), set, assets, -14, 12, -6, 4, 40);
        }
    }
}

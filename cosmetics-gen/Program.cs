// fn-cosmetics-gen: lists every cosmetic of a Fortnite build, with every style, straight from the build's own Paks.
// Output: the JSON the offline backend turns into the "athena" profile (all cosmetics owned, all styles owned).
//
// usage: fn-cosmetics-gen --build <build root> --out <file.json> [--aes <aes.json>] [--mappings <file.usmap>]
//                         [--engine 4.26] [--version 17.50]
// aes.json: {"mainKey":"0x..","dynamicKeys":[{"guid":"..","key":"0x.."}]} (dippyshere/fortnite-aes-archive format).
// Nothing here touches the network: keys and mappings are files given on the command line.
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider.Usmap;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Assets.Objects.Properties;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.GameplayTags;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.UE4.Versions;
using CUE4Parse.UE4.VirtualFileSystem;

static string? Arg(string[] a, string name) { var i = Array.IndexOf(a, name); return i >= 0 && i + 1 < a.Length ? a[i + 1] : null; }

var build = Arg(args, "--build") ?? throw new ArgumentException("--build missing");
var outPath = Arg(args, "--out") ?? throw new ArgumentException("--out missing");
var aesPath = Arg(args, "--aes");
var mappingsPath = Arg(args, "--mappings");
var engine = Arg(args, "--engine");
var version = Arg(args, "--version") ?? "";
var paks = Path.Combine(build, "FortniteGame", "Content", "Paks");
if (!Directory.Exists(paks)) { Console.Error.WriteLine($"no Paks folder: {paks}"); return 2; }

// Engine version -> CUE4Parse game enum (GAME_UE4_26, GAME_UE5_1, ...); the launcher reads it from the exe.
var game = EGame.GAME_UE4_26;
if (engine is not null && Regex.Match(engine, @"^(\d)\.(\d+)") is { Success: true } m
    && Enum.TryParse<EGame>($"GAME_UE{m.Groups[1].Value}_{m.Groups[2].Value}", out var g)) game = g;
Console.WriteLine($"engine {engine ?? "?"} -> {game}");

// No OodleHelper.Initialize(): without a native Oodle it would download one; the managed decoder (OodleSharp) is used.
// No ZlibHelper.Initialize(): it would download zlib-ng2.dll from the internet; Fortnite Paks use Oodle.

var provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, true, new VersionContainer(game));
if (mappingsPath is not null && File.Exists(mappingsPath)) provider.MappingsContainer = new FileUsmapTypeMappingsProvider(mappingsPath);
provider.Initialize();

var keys = new Dictionary<FGuid, FAesKey>();
var keyHex = new Dictionary<FGuid, string>();
if (aesPath is not null && File.Exists(aesPath))
{
    var aes = JsonNode.Parse(File.ReadAllText(aesPath))!;
    if (aes["mainKey"]?.GetValue<string>() is { Length: > 0 } mk) keys[new FGuid()] = new FAesKey(mk);
    foreach (var dk in aes["dynamicKeys"]?.AsArray() ?? [])
        if (dk?["guid"]?.GetValue<string>() is { } guid && dk["key"]?.GetValue<string>() is { } key)
            { keys[new FGuid(guid)] = new FAesKey(key); keyHex[new FGuid(guid)] = key; }
}
else keys[new FGuid()] = new FAesKey("0x0000000000000000000000000000000000000000000000000000000000000000");
provider.SubmitKeys(keys);
provider.PostMount();
Console.WriteLine($"mounted {provider.MountedVfs.Count} archives, {provider.UnloadedVfs.Count} still encrypted, {provider.Files.Count} files");
if (provider.MountedVfs.Count == 0) { Console.Error.WriteLine("nothing mounted: wrong or missing AES key"); return 3; }

// --extract <file path in the Paks> <output file>: developer aid (e.g. FortniteGame/Config/DefaultEngine.ini).
if (Arg(args, "--extract") is { } extract)
{
    var outFile = args[Array.IndexOf(args, "--extract") + 2];
    if (provider.TrySaveAsset(extract, out var bytes)) { File.WriteAllBytes(outFile, bytes); Console.WriteLine($"extracted {bytes.Length} bytes"); return 0; }
    Console.WriteLine($"not found: {extract}; candidates: " + string.Join(", ", provider.Files.Keys.Where(k => k.EndsWith(Path.GetFileName(extract), StringComparison.OrdinalIgnoreCase)).Take(10)));
    return 1;
}

// --textures <package path>=<out file>[;...]: decode Texture2D assets of the build to PNG/JPG (by extension),
// with the managed decoders (no native libraries). Used for the playlist tiles the backend serves.
if (Arg(args, "--textures") is { } texArg) { ExportTextures(provider, texArg.Split(';', StringSplitOptions.RemoveEmptyEntries).Select(p => (p.Split('=')[0], p.Split('=')[1]))); return 0; }

// --find <a;b;...> <out.txt>: developer aid, every file path in the Paks containing all the given substrings.
if (Arg(args, "--find") is { } find)
{
    var outFile = args[Array.IndexOf(args, "--find") + 2];
    var parts = find.Split(';', StringSplitOptions.RemoveEmptyEntries);
    var hits = provider.Files.Keys.Where(k => parts.All(p => k.Contains(p, StringComparison.OrdinalIgnoreCase))).OrderBy(k => k).ToList();
    File.WriteAllLines(outFile, hits);
    Console.WriteLine($"{hits.Count} files -> {outFile}");
    return 0;
}

// --playlists <out.json>: every FortPlaylistAthena of the build with its own display texts and properties.
if (Arg(args, "--playlists") is { } plOut)
{
    var list = new List<Dictionary<string, object?>>();
    foreach (var f in provider.Files.Keys.Where(k => k.Contains("/Playlists/", StringComparison.OrdinalIgnoreCase)
                 && Path.GetFileName(k).StartsWith("Playlist_", StringComparison.OrdinalIgnoreCase) && k.EndsWith(".uasset")).OrderBy(k => k))
    {
        var pkgPath = f[..^".uasset".Length];
        if (!provider.TryLoadPackage(pkgPath, out var pp)) continue;
        foreach (var ex in pp.GetExports())
        {
            if (!ex.ExportType.Contains("Playlist")) continue;
            var d = new Dictionary<string, object?> { ["path"] = pkgPath, ["class"] = ex.ExportType, ["name"] = ex.Name };
            foreach (var prop in ex.Properties)
            {
                string val;
                try { val = Newtonsoft.Json.JsonConvert.SerializeObject(prop.Tag); } catch (Exception e) { val = "ERR " + e.GetType().Name; }
                if (val.Length > 400) val = val[..400] + "...";
                d[prop.Name.Text] = val;
            }
            list.Add(d);
        }
    }
    File.WriteAllText(plOut, System.Text.Json.JsonSerializer.Serialize(list, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
    Console.WriteLine($"{list.Count} playlists -> {plOut}");
    return 0;
}

// --dump <package path>[,<package path>...]: developer aid, prints the ItemVariants objects of those packages as JSON.
if (Arg(args, "--dump") is { } dump)
{
    foreach (var pkgPath in dump.Split(','))
    {
        if (!provider.TryLoadPackage(pkgPath, out var dp)) { Console.WriteLine($"cannot load {pkgPath}"); continue; }
        foreach (var ex in dp.GetExports())
        {
            if (!ex.ExportType.Contains("ItemDefinition") && !ex.ExportType.Contains("Definition")) continue;
            Console.WriteLine($"== {pkgPath} {ex.ExportType} {ex.Name}");
            try
            {
                foreach (var v in ex.GetOrDefault("ItemVariants", Array.Empty<UObject>()))
                {
                    if (v is null) { Console.WriteLine("null variant"); continue; }
                    Console.WriteLine($"-- {v.ExportType}");
                    foreach (var prop in v.Properties)
                    {
                        string val;
                        try { val = Newtonsoft.Json.JsonConvert.SerializeObject(prop.Tag); } catch (Exception e) { val = "ERR " + e.GetType().Name; }
                        if (val.Length > 300) val = val[..300] + "...";
                        Console.WriteLine($"   {prop.Name} [{prop.PropertyType}] = {val}");
                    }
                    for (var t = provider.MappingsForGame?.Types.GetValueOrDefault(v.ExportType); t != null; t = t.Super.Value)
                        Console.WriteLine($"   class {t.Name}: " + string.Join(", ", t.Properties.Values.Select(pi => $"{pi.Name}:{pi.MappingType.Type}{(pi.MappingType.StructType is { } st ? "<" + st + ">" : "")}")));
                }
            }
            catch (Exception e) { Console.WriteLine("ERR " + e); }
        }
    }
    return 0;
}

// Item definition class -> MCP templateId prefix (the type the game's profile uses for that class).
var prefixes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
{
    ["AthenaCharacterItemDefinition"] = "AthenaCharacter",
    ["AthenaBackpackItemDefinition"] = "AthenaBackpack",
    ["AthenaPetCarrierItemDefinition"] = "AthenaPetCarrier",
    ["AthenaPetItemDefinition"] = "AthenaPet",
    ["AthenaPickaxeItemDefinition"] = "AthenaPickaxe",
    ["AthenaGliderItemDefinition"] = "AthenaGlider",
    ["AthenaSkyDiveContrailItemDefinition"] = "AthenaSkyDiveContrail",
    ["AthenaDanceItemDefinition"] = "AthenaDance",
    ["AthenaEmojiItemDefinition"] = "AthenaDance",
    ["AthenaSprayItemDefinition"] = "AthenaDance",
    ["AthenaToyItemDefinition"] = "AthenaDance",
    ["AthenaItemWrapDefinition"] = "AthenaItemWrap",
    ["AthenaLoadingScreenItemDefinition"] = "AthenaLoadingScreen",
    ["AthenaMusicPackItemDefinition"] = "AthenaMusicPack",
};

static string Last(string tag) { var i = tag.LastIndexOf('.'); return i >= 0 ? tag[(i + 1)..] : tag; }
// MCP names of variants, as the 17.50 client builds them (strings in its executable): the channel is the tag after
// "Cosmetics.Variant.Channel." (it rebuilds the tag with "Cosmetics.Variant.Channel.%s") and an option is the tag
// after "Cosmetics.Variant.Property." (e.g. "Mat1", "Color.000", "Numeric.23").
static string McpName(string tag, string prefix) => tag.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? tag[prefix.Length..] : Last(tag);
const string ChannelPrefix = "Cosmetics.Variant.Channel.", PropertyPrefix = "Cosmetics.Variant.Property.";
static string? TagOf(IPropertyHolder o, string name)
{
    if (o is UObject uo && uo.TryGetValue(out FGameplayTag t, name)) return t.TagName.Text;
    if (o is FStructFallback sf && sf.TryGetValue(out FGameplayTag t2, name)) return t2.TagName.Text;
    var s = o is UObject u ? u.GetOrDefault<FStructFallback?>(name) : ((FStructFallback) o).GetOrDefault<FStructFallback?>(name);
    return s?.GetOrDefault<FName?>("TagName")?.Text;
}

var items = new List<JsonObject>();
var keychain = new Dictionary<FGuid, string>(); // dynamic (per-cosmetic) pak -> one item it contains
var counts = new SortedDictionary<string, int>();
int scanned = 0, failed = 0, variantErrors = 0;
var unreadChannels = new List<string>();
var candidates = provider.Files.Values
    .Where(f => f.Extension.Equals("uasset", StringComparison.OrdinalIgnoreCase))
    .Where(f => f.Path.Contains("/Items/Cosmetics/", StringComparison.OrdinalIgnoreCase)
             || f.Path.Contains("/Cosmetics/", StringComparison.OrdinalIgnoreCase) && f.Path.Contains("Athena", StringComparison.OrdinalIgnoreCase))
    .DistinctBy(f => f.PathWithoutExtension, StringComparer.OrdinalIgnoreCase)
    .ToList();
Console.WriteLine($"{candidates.Count} candidate packages");

foreach (var file in candidates)
{
    scanned++;
    try
    {
        if (!provider.TryLoadPackage(file, out var pkg)) { failed++; continue; }
        foreach (var export in pkg.GetExports())
        {
            if (!prefixes.TryGetValue(export.ExportType, out var prefix)) continue;
            var variants = new JsonArray();
            UObject[] itemVariants;
            try { itemVariants = export.GetOrDefault("ItemVariants", Array.Empty<UObject>()); }
            catch (Exception e) { variantErrors++; if (variantErrors <= 20) Console.Error.WriteLine($"{file.Path}: styles not read ({e.Message})"); itemVariants = []; }
            foreach (var variant in itemVariants)
            try
            {
                if (variant is null) continue;
                var channelTag = TagOf(variant, "VariantChannelTag");
                // Not serialized when it equals the class default: the colour picker's default channel is
                // "Cosmetics.Variant.Channel.RichColor" (tag in the 17.50 exe); others have no default channel.
                if (string.IsNullOrEmpty(channelTag) && variant.ExportType == "FortCosmeticRichColorVariant") channelTag = "Cosmetics.Variant.Channel.RichColor";
                if (string.IsNullOrEmpty(channelTag)) { unreadChannels.Add($"{file.PathWithoutExtension} {variant.ExportType} (no channel tag)"); continue; }
                var owned = new List<string>();
                string? active = null, firstOption = null;
                if (variant.ExportType == "FortCosmeticNumericalVariant")
                {
                    // Numbers (jerseys). Min/Max/Default are native defaults unless the asset overrides them:
                    // UFortCosmeticNumericalVariant's constructor in the 17.50 exe (rva 0x1ae4e5a) sets Default 14, Min 0,
                    // Max 99. The client names them "Numeric.%i" and the range "Numeric.%i.%i" (min.max).
                    int min = variant.GetOrDefault("MinNumericalValue", 0), max = variant.GetOrDefault("MaxNumbericalValue", 99);
                    int def = variant.GetOrDefault("DefaultStartingNumeric", 14);
                    owned.Add($"Numeric.{min}.{max}");
                    for (var n = min; n <= max; n++) owned.Add($"Numeric.{n}");
                    active = $"Numeric.{def}";
                }
                else if (variant.ExportType == "FortCosmeticFloatSliderVariant")
                {
                    // Sliders (corruption). Native defaults (constructor at rva 0x1ae5831 in the 17.50 exe): Min 0, Max 100;
                    // the client formats values with "%.4f": "FloatSlider.%s" and the range "FloatSlider.%s-%s".
                    float min = variant.GetOrDefault("MinParamValue", 0f), max = variant.GetOrDefault("MaxParamValue", 100f);
                    float def = variant.GetOrDefault("DefaultStartingValue", 0f);
                    string F(float v) => Math.Clamp(v, min, max).ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture);
                    owned.Add($"FloatSlider.{F(min)}-{F(max)}");
                    active = $"FloatSlider.{F(def)}";
                }
                else if (variant.ExportType == "FortCosmeticRichColorVariant")
                {
                    // Colour pickers. The 17.50 client writes the colour as "RichColor.%s" with "%08X%08X%08X%08X" of the
                    // float bits of R,G,B,A (exe: rva 0x4b44d7c -> 0x18f7f90). Default = DefaultStartingColor; the choices
                    // are the colours of its ColorSwatchForChoices.
                    static string Hex(FLinearColor c) => $"{BitConverter.SingleToUInt32Bits(c.R):X8}{BitConverter.SingleToUInt32Bits(c.G):X8}{BitConverter.SingleToUInt32Bits(c.B):X8}{BitConverter.SingleToUInt32Bits(c.A):X8}";
                    if (variant.TryGetValue(out FStructFallback inline, "InlineVariant") && inline.TryGetValue(out FStructFallback rich, "RichColorVar"))
                    {
                        if (rich.TryGetValue(out FLinearColor def, "DefaultStartingColor")) active = $"RichColor.{Hex(def)}";
                        if (rich.TryGetValue(out FSoftObjectPath swatchPath, "ColorSwatchForChoices") && swatchPath.TryLoad(out UObject swatch)
                            && swatch.TryGetValue(out FStructFallback[] pairs, "ColorPairs"))
                            foreach (var pair in pairs) if (pair.TryGetValue(out FLinearColor cv, "ColorValue")) owned.Add($"RichColor.{Hex(cv)}");
                        if (active != null && !owned.Contains(active)) owned.Insert(0, active);
                    }
                }
                else if (variant.ExportType == "FortCosmeticItemTexture")
                {
                    // Item texture (e.g. a sticker picked from an owned emoticon): "ItemTexture.%s" with DefaultSelectedItem
                    // (exe: rva 0x4b44c9a). Any owned item of the allowed type can be picked; everything is owned here.
                    if (variant.TryGetValue(out FStructFallback itv, "ItemTextureVar") && itv.TryGetValue(out FStructFallback inner, "InnerDef"))
                    { active = $"ItemTexture.{inner.GetOrDefault("DefaultSelectedItem", "")}"; owned.Add(active); }
                }
                else if (variant.ExportType == "FortCosmeticVariantApplyWrap")
                {
                    // Applied wrap (the equipped wrap tints the clothing): "AppliedWrap.%s" (exe: rva 0x4b454aa) with the
                    // chosen wrap; the default is none. Any owned wrap can be applied; everything is owned here.
                    active = "AppliedWrap."; owned.Add(active);
                }
                else foreach (var prop in variant.Properties)
                {
                    if (prop.Tag is not ArrayProperty || prop.Tag.GetValue(typeof(FStructFallback[])) is not FStructFallback[] options) continue;
                    foreach (var opt in options)
                    {
                        var tag = TagOf(opt, "CustomizationVariantTag");
                        if (tag is null) continue;
                        var name = McpName(tag, PropertyPrefix);
                        owned.Add(name);
                        firstOption ??= name;
                        if (active is null && opt.GetOrDefault<bool>("bIsDefault")) active = name;
                    }
                }
                active ??= firstOption;
                if (owned.Count == 0) { unreadChannels.Add($"{file.PathWithoutExtension} {variant.ExportType} {channelTag}"); continue; }
                if (owned.Count == 0) continue;
                variants.Add(new JsonObject
                {
                    ["channel"] = McpName(channelTag, ChannelPrefix),
                    ["active"] = active ?? owned[0],
                    ["owned"] = new JsonArray(owned.Distinct().Select(o => (JsonNode) o).ToArray())
                });
            }
            catch (Exception e) { variantErrors++; if (variantErrors <= 20) Console.Error.WriteLine($"{file.Path}: a style channel not read ({e.Message})"); }
            items.Add(new JsonObject { ["templateId"] = $"{prefix}:{export.Name.ToLowerInvariant()}", ["variants"] = variants });
            if (file is VfsEntry { Vfs: IAesVfsReader aesVfs } && aesVfs.EncryptionKeyGuid != new FGuid() && !keychain.ContainsKey(aesVfs.EncryptionKeyGuid))
                keychain[aesVfs.EncryptionKeyGuid] = $"{prefix}:{export.Name.ToLowerInvariant()}";
            counts[prefix] = counts.GetValueOrDefault(prefix) + 1;
            break;
        }
    }
    catch (Exception e) { failed++; if (failed <= 20) Console.Error.WriteLine($"{file.Path}: {e.Message}"); }
    if (scanned % 1000 == 0) Console.WriteLine($"{scanned}/{candidates.Count} scanned, {items.Count} items");
}

var result = new JsonObject
{
    ["version"] = version,
    ["engine"] = engine,
    ["generatedAt"] = DateTime.UtcNow.ToString("o"),
    ["source"] = "build Paks (fn-cosmetics-gen)",
    ["counts"] = JsonSerializer.SerializeToNode(counts),
    ["failedPackages"] = failed,
    ["styleReadErrors"] = variantErrors,
    ["unreadStyleChannels"] = new JsonArray(unreadChannels.Select(u => (JsonNode) u).ToArray()),
    ["items"] = new JsonArray(items.DistinctBy(i => i["templateId"]!.GetValue<string>()).Select(i => (JsonNode) i).ToArray())
};
// Keychain (what Epic's /storefront/v2/keychain serves): "GUID:base64(key):item" so the client mounts the
// per-cosmetic encrypted paks at login.
var chain = new JsonArray();
foreach (var (guid, item) in keychain)
    if (keyHex.TryGetValue(guid, out var hex))
        chain.Add($"{guid.ToString(EGuidFormats.Digits).ToUpperInvariant()}:{Convert.ToBase64String(Convert.FromHexString(hex.Replace("0x", "")))}:{item}");
File.WriteAllText(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(outPath))!, "keychain.json"), chain.ToJsonString());
Console.WriteLine($"keychain: {chain.Count} dynamic paks");
File.WriteAllText(outPath, result.ToJsonString(new JsonSerializerOptions { WriteIndented = false }));
Console.WriteLine($"done: {items.Count} cosmetics ({string.Join(", ", counts.Select(c => $"{c.Key} {c.Value}"))}), {failed} packages failed, {variantErrors} style read errors, {unreadChannels.Count} channels without options -> {outPath}");
foreach (var u in unreadChannels.Take(40)) Console.WriteLine("  no options: " + u);
// The game's own art for the lobby mode tiles the backend lists (offline\ui\br.jpg, cm.jpg): the Battle Royale and
// Creative pictures of the build's "select a game mode" screen. Missing in a build = tiles without picture.
var uiDir = Directory.CreateDirectory(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(outPath))!, "ui")).FullName;
ExportTextures(provider, new[] {
    ("FortniteGame/Content/UI/Login/SubGameSelect/Textures/T_SubgameSelect_V2_Default_BR", Path.Combine(uiDir, "br.jpg")),
    ("FortniteGame/Content/UI/Login/SubGameSelect/Textures/T_SubgameSelect_V2_Default_CM", Path.Combine(uiDir, "cm.jpg")) });
return items.Count > 0 ? 0 : 4;

// Decodes Texture2D assets of the build to JPG/PNG (by extension) with the managed decoders (no native libraries).
static void ExportTextures(DefaultFileProvider provider, IEnumerable<(string pkg, string dst)> list)
{
    CUE4Parse_Conversion.Textures.TextureDecoder.UseAssetRipperTextureDecoder = true;
    foreach (var (pkg, dst) in list)
    {
        try
        {
            if (!provider.TryLoadPackageObject<CUE4Parse.UE4.Assets.Exports.Texture.UTexture2D>(pkg, out var tex)) { Console.WriteLine($"no texture {pkg}"); continue; }
            var bmp = CUE4Parse_Conversion.Textures.TextureDecoder.Decode(tex, 1024) ?? throw new Exception("decode failed");
            using var sk = CUE4Parse_Conversion.Textures.TextureEncoder.ToSkBitmap(bmp);
            var fmt = dst.EndsWith(".png", StringComparison.OrdinalIgnoreCase) ? SkiaSharp.SKEncodedImageFormat.Png : SkiaSharp.SKEncodedImageFormat.Jpeg;
            using var data = sk.Encode(fmt, 88);
            File.WriteAllBytes(dst, data.ToArray());
            Console.WriteLine($"{pkg} -> {dst} ({sk.Width}x{sk.Height})");
        }
        catch (Exception e) { Console.WriteLine($"{pkg}: {e.Message}"); }
    }
}

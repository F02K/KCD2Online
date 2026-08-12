using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Text.Json.Serialization;

try
{
    Console.Title = "KCD2Online Version Manager";
    var options = Options.Parse(args);
    if (options.WaitPid is not null)
    {
        Console.WriteLine($"Waiting for process {options.WaitPid} to exit...");
        try
        {
            using var process = Process.GetProcessById(options.WaitPid.Value);
            await process.WaitForExitAsync();
        }
        catch (ArgumentException)
        {
        }
    }

    using var http = new HttpClient
    {
        BaseAddress = new Uri(options.Api.TrimEnd('/') + "/"),
        Timeout = TimeSpan.FromMinutes(20)
    };
    http.DefaultRequestHeaders.UserAgent.ParseAdd("KCD2OnlineBootstrap/1.0");
    var verified = await FetchAndVerifyManifestAsync(http, options.Version);
    if (!verified.Artifacts.TryGetValue(options.Component, out var artifact))
        throw new InvalidDataException($"Release {options.Version} contains no {options.Component} artifact.");

    var cacheRoot = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KCD2Online", "release-cache", options.Component, options.Version);
    Directory.CreateDirectory(cacheRoot);
    var archivePath = Path.Combine(cacheRoot, artifact.FileName);
    await DownloadAsync(http, artifact.DownloadPath, archivePath, artifact.Size, artifact.Sha256);

    var extractedRoot = Path.Combine(cacheRoot, "files");
    await ExtractAndVerifyAsync(
        archivePath, extractedRoot, options.Version, options.Component, artifact.Files);
    await ActivateAsync(extractedRoot, options.TargetRoot, options.Version, options.Component);
    Console.WriteLine($"KCD2Online {options.Component} {options.Version} is active.");

    if (options.Relaunch && options.Component == "client")
    {
        var game = Path.Combine(
            options.TargetRoot, "Bin", "Win64MasterMasterSteamPGO", "KingdomCome.exe");
        if (!File.Exists(game))
            throw new FileNotFoundException("KingdomCome.exe was not found after the update.", game);
        Process.Start(new ProcessStartInfo(game) { UseShellExecute = true });
    }
    else if (options.Relaunch && options.Component == "server")
    {
        var server = Path.Combine(options.TargetRoot, "KCD2OnlineServer.exe");
        if (!File.Exists(server))
            throw new FileNotFoundException("KCD2OnlineServer.exe was not found after the update.", server);
        var start = new ProcessStartInfo(server)
        {
            UseShellExecute = true,
            WorkingDirectory = options.TargetRoot
        };
        var config = Path.Combine(options.TargetRoot, "server.toml");
        if (File.Exists(config))
            start.ArgumentList.Add(config);
        Process.Start(start);
    }
}
catch (Exception exception)
{
    Console.Error.WriteLine($"KCD2Online update failed: {exception.Message}");
    if (!Console.IsInputRedirected)
    {
        Console.Error.WriteLine("Press any key to close.");
        Console.ReadKey(true);
    }
    Environment.ExitCode = 1;
}

static async Task<ManifestPayload> FetchAndVerifyManifestAsync(
    HttpClient http,
    string version)
{
    var document = await http.GetFromJsonAsync(
        $"v1/releases/{version}", BootstrapJsonContext.Default.SignedManifest)
        ?? throw new InvalidDataException($"Release {version} was not found.");
    var key = await http.GetFromJsonAsync(
        "v1/auth/signing-key", BootstrapJsonContext.Default.SigningKey)
        ?? throw new InvalidDataException("The backend signing key is unavailable.");
    if (!string.Equals(document.Algorithm, "ES256", StringComparison.Ordinal)
        || !string.Equals(document.KeyId, key.KeyId, StringComparison.Ordinal))
        throw new CryptographicException("The release signing key does not match the backend.");
    var payloadBytes = Base64UrlDecode(document.Payload);
    var signature = Base64UrlDecode(document.Signature);
    using var verifier = ECDsa.Create();
    verifier.ImportSubjectPublicKeyInfo(Base64UrlDecode(key.PublicKeySpki), out var bytesRead);
    if (bytesRead == 0
        || !verifier.VerifyData(
            payloadBytes,
            signature,
            HashAlgorithmName.SHA256,
            DSASignatureFormat.IeeeP1363FixedFieldConcatenation))
        throw new CryptographicException("The release manifest signature is invalid.");
    var payload = JsonSerializer.Deserialize(
        payloadBytes, BootstrapJsonContext.Default.ManifestPayload)
        ?? throw new InvalidDataException("The release manifest payload is invalid.");
    if (payload.ManifestFormat != 1 || !string.Equals(payload.Version, version, StringComparison.Ordinal))
        throw new InvalidDataException("The release manifest version is invalid.");
    return payload;
}

static async Task DownloadAsync(
    HttpClient http,
    string url,
    string path,
    long expectedSize,
    string expectedSha256)
{
    if (File.Exists(path) && new FileInfo(path).Length == expectedSize)
    {
        if (string.Equals(
            await HashFileAsync(path), expectedSha256, StringComparison.OrdinalIgnoreCase))
            return;
        File.Delete(path);
    }
    var partial = path + ".part";
    var offset = File.Exists(partial) ? new FileInfo(partial).Length : 0;
    if (offset > expectedSize)
    {
        File.Delete(partial);
        offset = 0;
    }
    using var request = new HttpRequestMessage(HttpMethod.Get, url);
    if (offset > 0)
        request.Headers.Range = new RangeHeaderValue(offset, null);
    using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
    if (offset > 0 && response.StatusCode != HttpStatusCode.PartialContent)
    {
        File.Delete(partial);
        offset = 0;
    }
    response.EnsureSuccessStatusCode();
    await using var source = await response.Content.ReadAsStreamAsync();
    await using var target = new FileStream(
        partial, offset == 0 ? FileMode.Create : FileMode.Append,
        FileAccess.Write, FileShare.None, 128 * 1024, true);
    var buffer = new byte[128 * 1024];
    long total = offset;
    var lastPercentage = -1;
    for (;;)
    {
        var read = await source.ReadAsync(buffer);
        if (read == 0)
            break;
        await target.WriteAsync(buffer.AsMemory(0, read));
        total += read;
        var percentage = (int)(100 * total / expectedSize);
        if (percentage != lastPercentage)
        {
            Console.Write($"\rDownloading: {percentage,3}%");
            lastPercentage = percentage;
        }
    }
    Console.WriteLine();
    await target.FlushAsync();
    if (total != expectedSize)
        throw new InvalidDataException("The release download has an unexpected size.");
    target.Close();
    File.Move(partial, path, true);
    if (!string.Equals(
        await HashFileAsync(path), expectedSha256, StringComparison.OrdinalIgnoreCase))
    {
        File.Delete(path);
        throw new InvalidDataException("The downloaded release archive hash is invalid.");
    }
}

static async Task ExtractAndVerifyAsync(
    string archivePath,
    string outputRoot,
    string version,
    string component,
    IReadOnlyList<ReleaseFile> expectedFiles)
{
    var temporary = outputRoot + ".new-" + Guid.NewGuid().ToString("N");
    Directory.CreateDirectory(temporary);
    var expected = expectedFiles.ToDictionary(x => x.Path, StringComparer.OrdinalIgnoreCase);
    var found = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    try
    {
        using var archive = ZipFile.OpenRead(archivePath);
        var prefix = component == "client"
            ? "KingdomComeDeliverance2/"
            : $"KCD2Online-Server-v{version}/";
        foreach (var entry in archive.Entries)
        {
            var archiveName = entry.FullName.Replace('\\', '/');
            if (archiveName.EndsWith('/'))
                continue;
            if (!expected.TryGetValue(archiveName, out var declared)
                || !found.Add(archiveName)
                || !archiveName.StartsWith(prefix, StringComparison.Ordinal))
                throw new InvalidDataException("The archive does not match its signed file manifest.");
            var relative = archiveName[prefix.Length..];
            if (!SafeRelativePath(relative))
                throw new InvalidDataException("The archive contains an unsafe path.");
            var destination = Path.GetFullPath(Path.Combine(temporary, relative.Replace('/', Path.DirectorySeparatorChar)));
            if (!destination.StartsWith(Path.GetFullPath(temporary) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("The archive path escapes the staging directory.");
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            await using (var source = entry.Open())
            await using (var target = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 128 * 1024, true))
                await source.CopyToAsync(target);
            if (new FileInfo(destination).Length != declared.Size
                || !string.Equals(await HashFileAsync(destination), declared.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"File verification failed: {archiveName}");
        }
        if (found.Count != expected.Count)
            throw new InvalidDataException("The archive is missing signed files.");
        if (Directory.Exists(outputRoot))
            Directory.Delete(outputRoot, true);
        Directory.Move(temporary, outputRoot);
    }
    catch
    {
        if (Directory.Exists(temporary))
            Directory.Delete(temporary, true);
        throw;
    }
}

static async Task ActivateAsync(
    string sourceRoot,
    string targetRoot,
    string version,
    string component)
{
    targetRoot = Path.GetFullPath(targetRoot);
    if (!Directory.Exists(targetRoot))
        throw new DirectoryNotFoundException($"Update target does not exist: {targetRoot}");
    var stateRoot = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KCD2Online", "installations", RootId(targetRoot));
    var receiptPath = Path.Combine(stateRoot, "current.json");
    InstallReceipt? previous = null;
    if (File.Exists(receiptPath))
    {
        try
        {
            previous = JsonSerializer.Deserialize(
                await File.ReadAllTextAsync(receiptPath),
                BootstrapJsonContext.Default.InstallReceipt);
            if (previous is null
                || previous.Format != 1
                || !string.Equals(previous.Component, component, StringComparison.Ordinal)
                || !string.Equals(
                    Path.GetFullPath(previous.TargetRoot), targetRoot,
                    StringComparison.OrdinalIgnoreCase))
                previous = null;
        }
        catch
        {
            previous = null;
        }
    }
    var backupRoot = Path.Combine(stateRoot, "backups", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds().ToString());
    Directory.CreateDirectory(backupRoot);
    var applied = new List<string>();
    var backedUp = new List<string>();
    var removed = new List<string>();
    var sources = Directory.EnumerateFiles(sourceRoot, "*", SearchOption.AllDirectories)
        .Select(source => (Source: source, Relative: Path.GetRelativePath(sourceRoot, source)))
        .ToList();
    var newFiles = sources.Select(x => x.Relative).ToHashSet(StringComparer.OrdinalIgnoreCase);
    try
    {
        foreach (var item in sources)
        {
            var source = item.Source;
            var relative = item.Relative;
            if (!SafeRelativePath(relative.Replace('\\', '/')))
                throw new InvalidDataException("The staged release contains an unsafe path.");
            var destination = Path.GetFullPath(Path.Combine(targetRoot, relative));
            if (!destination.StartsWith(targetRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("An update target escapes the installation directory.");
            if (string.Equals(
                destination, Environment.ProcessPath, StringComparison.OrdinalIgnoreCase))
                continue;
            if (File.Exists(destination))
            {
                var backup = Path.Combine(backupRoot, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                File.Copy(destination, backup, true);
                backedUp.Add(relative);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            var temporary = destination + ".kcd2online-new";
            File.Copy(source, temporary, true);
            File.Move(temporary, destination, true);
            applied.Add(relative);
        }
        foreach (var relative in previous?.Files ?? [])
        {
            if (newFiles.Contains(relative) || !SafeRelativePath(relative.Replace('\\', '/')))
                continue;
            var destination = Path.GetFullPath(Path.Combine(targetRoot, relative));
            if (!destination.StartsWith(targetRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                || !File.Exists(destination))
                continue;
            var backup = Path.Combine(backupRoot, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
            File.Copy(destination, backup, true);
            File.Delete(destination);
            backedUp.Add(relative);
            removed.Add(relative);
        }
        Directory.CreateDirectory(stateRoot);
        var receipt = new InstallReceipt(1, component, version, targetRoot, applied, backedUp, backupRoot);
        var temporaryReceipt = receiptPath + ".tmp";
        await File.WriteAllTextAsync(
            temporaryReceipt,
            JsonSerializer.Serialize(receipt, BootstrapJsonContext.Default.InstallReceipt));
        File.Move(temporaryReceipt, receiptPath, true);
    }
    catch
    {
        var backedUpSet = backedUp.ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var relative in removed.AsEnumerable().Reverse())
        {
            try
            {
                var destination = Path.Combine(targetRoot, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.Copy(Path.Combine(backupRoot, relative), destination, true);
            }
            catch
            {
                // Keep rolling back other files and preserve the original error.
            }
        }
        foreach (var relative in applied.AsEnumerable().Reverse())
        {
            var destination = Path.Combine(targetRoot, relative);
            try
            {
                if (backedUpSet.Contains(relative))
                    File.Copy(Path.Combine(backupRoot, relative), destination, true);
                else if (File.Exists(destination))
                    File.Delete(destination);
            }
            catch
            {
                // Preserve the original activation error. The backup remains on disk
                // for manual recovery if Windows also rejected the rollback write.
            }
        }
        throw;
    }
}

static bool SafeRelativePath(string path) =>
    !string.IsNullOrWhiteSpace(path)
    && !Path.IsPathRooted(path)
    && path.Split('/', StringSplitOptions.RemoveEmptyEntries).All(x => x is not "." and not "..");

static string RootId(string root) =>
    Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(root.ToUpperInvariant())))[..16].ToLowerInvariant();

static async Task<string> HashFileAsync(string path)
{
    await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 128 * 1024, true);
    return Convert.ToHexString(await SHA256.HashDataAsync(input)).ToLowerInvariant();
}

static byte[] Base64UrlDecode(string text)
{
    var normalized = text.Replace('-', '+').Replace('_', '/');
    normalized += (normalized.Length % 4) switch
    {
        0 => string.Empty,
        2 => "==",
        3 => "=",
        _ => throw new FormatException("Invalid base64url data.")
    };
    return Convert.FromBase64String(normalized);
}

internal sealed record Options(
    string Component,
    string Version,
    string TargetRoot,
    string Api,
    int? WaitPid,
    bool Relaunch)
{
    private static readonly Regex VersionPattern = new(
        "^[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?$",
        RegexOptions.CultureInvariant);

    public static Options Parse(string[] args)
    {
        if (args.Length < 3 || args[0] is not ("client" or "server"))
            throw new ArgumentException(
                "Usage: KCD2OnlineBootstrap <client|server> <version> <target-root> [--wait-pid N] [--relaunch] [--api URL]");
        if (!VersionPattern.IsMatch(args[1]))
            throw new ArgumentException("The requested KCD2Online version is invalid.");
        var api = "https://api.kingdom-online.cc";
        int? waitPid = null;
        var relaunch = false;
        for (var index = 3; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--api": api = args[++index]; break;
                case "--wait-pid": waitPid = int.Parse(args[++index]); break;
                case "--relaunch": relaunch = true; break;
                default: throw new ArgumentException($"Unknown option: {args[index]}");
            }
        }
        return new(args[0], args[1], Path.GetFullPath(args[2]), api, waitPid, relaunch);
    }
}

internal sealed record SigningKey(
    string Algorithm,
    string KeyId,
    string PublicKeySpki,
    string PublicKeyPem);

internal sealed record SignedManifest(
    string Algorithm,
    string KeyId,
    string Payload,
    string Signature);

internal sealed record ManifestPayload(
    int ManifestFormat,
    string Version,
    long PublishedAtUnixMs,
    IReadOnlyDictionary<string, ReleaseArtifact> Artifacts);

internal sealed record ReleaseArtifact(
    string Component,
    string FileName,
    long Size,
    string Sha256,
    string DownloadPath,
    IReadOnlyList<ReleaseFile> Files);

internal sealed record ReleaseFile(string Path, long Size, string Sha256);

internal sealed record InstallReceipt(
    int Format,
    string Component,
    string Version,
    string TargetRoot,
    IReadOnlyList<string> Files,
    IReadOnlyList<string> BackedUpFiles,
    string BackupRoot);

[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    WriteIndented = true)]
[JsonSerializable(typeof(SigningKey))]
[JsonSerializable(typeof(SignedManifest))]
[JsonSerializable(typeof(ManifestPayload))]
[JsonSerializable(typeof(InstallReceipt))]
internal sealed partial class BootstrapJsonContext : JsonSerializerContext;

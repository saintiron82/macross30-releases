using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace MacrossLauncher {
public static class GameLanguage {
    public static readonly string[] Codes={"ko-KR","en-US","ja-JP"};
    public static string Normalize(string locale) { return Codes.Contains(locale) ? locale : "ko-KR"; }
    public static string Folder(string root,string locale) {
        if(!Codes.Contains(locale)) throw new InvalidDataException("Unsupported game language");
        string version=Packages.InstalledVersion(root); Packages.VersionOf(version);
        return Path.Combine(root,"languages",locale,version);
    }
    public static bool Ready(string root,string locale) {
        if(locale=="ko-KR") return Packages.ProgramReady(root) && Packages.DataReady(root);
        try {
            string path=Folder(root,locale);
            var m=Packages.Read(File.ReadAllText(Path.Combine(path,"language.json")));
            return Packages.ProgramReady(root) && File.ReadAllText(Path.Combine(root,"data/installed-version.txt")).Trim()==Packages.Text(m,"data_id") &&
                File.Exists(Path.Combine(path,"Macross30.cmd")) && Packages.Hash(Path.Combine(path,"EBOOT.elf"))==Packages.Text(m,"elf_sha256");
        } catch { return false; }
    }
    public static Dictionary<string,object> Select(Dictionary<string,object> catalog,string locale,string version) {
        if(!Codes.Contains(locale) || Packages.Text(catalog,"game_version")!=version) throw new InvalidDataException(UiText.T("게임 언어 정보가 버전과 맞지 않습니다."));
        var languages=Packages.Object(catalog["languages"]);
        if(!languages.ContainsKey(locale) || !Convert.ToBoolean(Packages.Object(languages[locale])["available"]))
            throw new InvalidOperationException(UiText.T("선택한 게임 언어팩은 아직 준비 중입니다. 다른 게임 언어를 선택하세요."));
        return Packages.Object(languages[locale]);
    }
    public static void ApplyAddon(string root,string locale,string archive,Dictionary<string,object> asset) {
        Packages.VerifyArchive(archive,asset); Packages.EnsureStopped(root);
        string destination=Folder(root,locale); Packages.NoLinks(root,"languages/"+locale);
        string parent=Path.GetDirectoryName(destination);Directory.CreateDirectory(parent);
        string stage=Path.Combine(parent,".stage-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(stage);
        string prefix="Macross30/languages/"+locale+"/"+Packages.InstalledVersion(root)+"/";
        try {
            var found=new HashSet<string>(StringComparer.OrdinalIgnoreCase); long total=0;
            using(var zip=ZipFile.OpenRead(archive)) foreach(var entry in zip.Entries) {
                if(!entry.FullName.StartsWith(prefix,StringComparison.Ordinal) || entry.FullName.EndsWith("/")) throw new InvalidDataException("Invalid language archive path");
                string rel=entry.FullName.Substring(prefix.Length);
                if(!new[]{"language.json","install-plan.tsv","EBOOT.mkdelta","install.ps1"}.Contains(rel) || !found.Add(rel) || ((entry.ExternalAttributes>>16)&0xF000)==0xA000) throw new InvalidDataException("Invalid language archive file");
                total+=entry.Length;if(total>32L*1024*1024) throw new InvalidDataException("Language archive too large");
                entry.ExtractToFile(Packages.Child(stage,rel));
            }
            if(found.Count!=4) throw new InvalidDataException("Language files missing");
            var m=Packages.Read(File.ReadAllText(Path.Combine(stage,"language.json")));
            if(Packages.Text(m,"locale")!=locale || Packages.Text(m,"game_version")!=Packages.InstalledVersion(root)) throw new InvalidDataException("Language version mismatch");
            var hashes=Packages.Object(m["files"]);
            foreach(string rel in found.Where(name=>name!="language.json")) if(Packages.Hash(Path.Combine(stage,rel))!=Convert.ToString(hashes[rel])) throw new InvalidDataException("Language file hash mismatch");
            Packages.NoLinks(root,"languages/"+locale+"/"+Packages.InstalledVersion(root));
            if(Directory.Exists(destination)) {
                if(File.ReadAllText(Path.Combine(destination,"language.json"))==File.ReadAllText(Path.Combine(stage,"language.json"))) return;
                string old=destination+".previous-"+Guid.NewGuid().ToString("N");Directory.Move(destination,old);
                try { Directory.Move(stage,destination); } catch { Directory.Move(old,destination);throw; }
            } else Directory.Move(stage,destination);
        } finally { if(Directory.Exists(stage)) Directory.Delete(stage,true); }
    }
    public static void Prepare(string root,string locale) {
        string path=Folder(root,locale); var m=Packages.Read(File.ReadAllText(Path.Combine(path,"language.json")));
        string source=Path.Combine(root,"bin/EBOOT.ko.elf"),delta=Path.Combine(path,"EBOOT.mkdelta");
        if(Packages.Hash(source)!=Packages.Text(m,"base_elf_sha256") || Packages.Hash(delta)!=Convert.ToString(Packages.Object(m["files"])["EBOOT.mkdelta"])) throw new InvalidDataException("Language ELF source mismatch");
        byte[] original=File.ReadAllBytes(source);var output=new MemoryStream();
        using(var file=File.OpenRead(delta)) using(var reader=new BinaryReader(file)) {
            if(Encoding.ASCII.GetString(reader.ReadBytes(8))!="MKDELTA1" || reader.ReadUInt32()!=1) throw new InvalidDataException("Invalid ELF delta");
            reader.ReadUInt32();long sourceSize=reader.ReadInt64();string sourceHash=Hex(reader.ReadBytes(32));
            long size=reader.ReadInt64();string hash=Hex(reader.ReadBytes(32));uint count=reader.ReadUInt32();
            if(sourceSize!=original.Length || sourceHash!=Packages.Hash(source) || size<=0 || size>32*1024*1024 || count>100000) throw new InvalidDataException("Invalid ELF delta bounds");
            var kinds=new byte[count];var offsets=new long[count];var lengths=new long[count];
            for(int i=0;i<count;i++) { kinds[i]=reader.ReadByte();offsets[i]=reader.ReadInt64();lengths[i]=reader.ReadInt64(); }
            long literalSize=reader.ReadInt64();if(literalSize!=file.Length-file.Position) throw new InvalidDataException("Invalid literal size");
            using(var literal=new DeflateStream(file,CompressionMode.Decompress,true)) for(int i=0;i<count;i++) {
                if(lengths[i]<0 || output.Length+lengths[i]>size) throw new InvalidDataException("Invalid ELF output size");
                if(kinds[i]==0) { if(offsets[i]<0 || offsets[i]+lengths[i]>original.Length) throw new InvalidDataException("Invalid ELF copy range");output.Write(original,(int)offsets[i],(int)lengths[i]); }
                else if(kinds[i]==1) { var buffer=new byte[65536];long left=lengths[i];while(left>0) { int n=literal.Read(buffer,0,(int)Math.Min(left,buffer.Length));if(n==0) throw new EndOfStreamException();output.Write(buffer,0,n);left-=n; } }
                else throw new InvalidDataException("Invalid ELF delta opcode");
            }
            byte[] data=output.ToArray();using(var sha=System.Security.Cryptography.SHA256.Create()) if(data.Length!=size || Hex(sha.ComputeHash(data))!=hash || hash!=Packages.Text(m,"elf_sha256")) throw new InvalidDataException("Language ELF output mismatch");
            File.WriteAllBytes(Path.Combine(path,"EBOOT.elf"),data);
        }
        string relative="languages\\"+locale+"\\"+Packages.InstalledVersion(root);
        string cmd=File.ReadAllText(Path.Combine(root,"Macross30.cmd"));
        cmd=cmd.Replace("set \"_M30_ROOT=%~dp0\"","set \"_M30_ROOT=%~dp0..\\..\\..\"")
            .Replace("\\bin\\EBOOT.ko.elf","\\"+relative+"\\EBOOT.elf")
            .Replace("\\korean\\data-version.txt","\\"+relative+"\\data-version.txt")
            .Replace("MACROSS_BATTLE_SUBTITLES=1","MACROSS_BATTLE_SUBTITLES=0")
            .Replace("\\bin\\subtitles","\\"+relative+"\\subtitles");
        Directory.CreateDirectory(Path.Combine(path,"subtitles"));
        File.WriteAllText(Path.Combine(path,"data-version.txt"),Packages.Text(m,"data_id")+"\n",new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(path,"Macross30.cmd"),cmd,new UTF8Encoding(false));
    }
    static string Hex(byte[] data) { return BitConverter.ToString(data).Replace("-","").ToLowerInvariant(); }
}
}

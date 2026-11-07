using System;
using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace MacrossLauncher {
    // Invoked only by the user's launcher Options button. The game owns the
    // settings UI and applies its existing runtime options on its window thread.
    public static class GameOptions {
        const uint WM_SYSCOMMAND = 0x0112;
        const uint OPEN_OPTIONS = 0x1F90; // rsx_null_backend.c, shipped game 0.3.8
        [DllImport("user32.dll", SetLastError=true)]
        static extern IntPtr SendMessageTimeoutW(IntPtr window,uint message,UIntPtr wparam,IntPtr lparam,uint flags,uint timeout,out UIntPtr result);
        [DllImport("user32.dll")]
        static extern bool IsIconic(IntPtr window);
        [DllImport("user32.dll")]
        static extern bool ShowWindow(IntPtr window,int command);
        [DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);

        public static bool MatchesInstallation(string root,string executable) {
            return String.Equals(Path.GetFullPath(Path.Combine(root,"bin/Macross30Recompiled.exe")),
                                 Path.GetFullPath(executable),StringComparison.OrdinalIgnoreCase);
        }

        public static void Open(string root) {
            root=Path.GetFullPath(root);
            Packages.NoLinks(root,"bin/Macross30Recompiled.exe");
            Process selected=null;
            try {
                foreach(var process in Process.GetProcessesByName("Macross30Recompiled")) {
                    bool keep=false;
                    try {
                        if(!MatchesInstallation(root,process.MainModule.FileName))continue;
                        if(selected!=null)throw new IOException("game_options_ambiguous");
                        selected=process;keep=true;
                    } catch(System.ComponentModel.Win32Exception) { /* unverified processes are never targeted */ }
                      catch(InvalidOperationException) { /* exited during inspection */ }
                    finally {if(!keep)process.Dispose();}
                }
                if(selected==null)throw new IOException("game_options_no_game");
                selected.Refresh();
                IntPtr window=selected.MainWindowHandle;
                uint pid;
                if(window==IntPtr.Zero || GetWindowThreadProcessId(window,out pid)==0 || pid!=(uint)selected.Id)
                    throw new IOException("game_options_unavailable");
                // A minimized owner has an off-screen rectangle; restore it so
                // the game's own centered options dialog remains visible.
                if(IsIconic(window))ShowWindow(window,9);
                UIntPtr result;
                if(SendMessageTimeoutW(window,WM_SYSCOMMAND,new UIntPtr(OPEN_OPTIONS),IntPtr.Zero,2,1500,out result)==IntPtr.Zero)
                    throw new IOException("game_options_timeout");
            } finally {if(selected!=null)selected.Dispose();}
        }
    }
}

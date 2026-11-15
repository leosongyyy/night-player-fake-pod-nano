using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using Un4seen.Bass;

namespace TestMP3
{
    internal static class Program
    {
        public static string RootDir
        {
            get
            {
                return Application.StartupPath;
            }
        }

        /// <summary>
        /// 应用程序的主入口点。
        /// </summary>
        [STAThread]
        static void Main()
        {
            var tempBass = Path.Combine(Path.GetTempPath(), "TestMP3PictureInfo", "bass");
            var tempPlugin = Path.Combine(tempBass, "plugin");
            if (!Directory.Exists(tempPlugin))
            {
                Directory.CreateDirectory(tempPlugin);
            }
            var bassDllFile = Path.Combine(tempBass, "bass.dll");
            var tagsDllFile = Path.Combine(tempPlugin, "tags.dll");
            if (!File.Exists(bassDllFile))
            {
                File.WriteAllBytes(bassDllFile + ".tmp", Properties.Resources.bass_dll);
                File.Move(bassDllFile + ".tmp", bassDllFile);
            }
            if (!File.Exists(tagsDllFile))
            {
                File.WriteAllBytes(tagsDllFile + ".tmp", Properties.Resources.tags_dll);
                File.Move(tagsDllFile + ".tmp", tagsDllFile);
            }
            Bass.LoadMe(tempBass);
            Bass.BASS_PluginLoad(tagsDllFile);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new Form1());
            
        }
    }
}

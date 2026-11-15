
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using Un4seen.Bass;
using Un4seen.Bass.AddOn.Tags;
using static System.Windows.Forms.VisualStyles.VisualStyleElement;

namespace TestMP3
{
    public partial class Form1 : Form
    {


        public Form1()
        {

            InitializeComponent();
            try
            {
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            }
            catch { Icon = SystemIcons.Application; }
            // Bass.LoadMe();
            Bass.BASS_Init(0, 48000, BASSInit.BASS_DEVICE_NOSPEAKER, IntPtr.Zero);

        }

        string[] supportExtension = {".mp3",".wav",".ogg",".flac" };

        private void btnDoScan_Click(object sender, EventArgs e)
        {
            btnDoScan.Enabled = false;
            print("========= 扫描开始 =========");
            bool scanOnly = chkScanOnly.Checked;
            Application.DoEvents();
            List<string> mp3File = Directory.EnumerateFiles(Program.RootDir, "*", SearchOption.AllDirectories).Where(s => supportExtension.Any(et => s.EndsWith(et,StringComparison.OrdinalIgnoreCase))).Where(s => !s.Contains("\\.")).OrderBy(s => s).ToList();

            var outDir = Path.Combine(Program.RootDir, ".ecov");
            if (!Directory.Exists(outDir))
            {
                Directory.CreateDirectory(outDir);
            }
            int total = mp3File.Count;
            int current = 0;
            foreach (var item in mp3File)
            {
                current++;
                var relPath = Path.GetFullPath(item).Substring(Path.GetFullPath(Program.RootDir).Length).TrimStart('/','\\');
                var metaFile = Path.Combine(outDir, relPath+".npd");
                bool needWrite = false;
                if (!File.Exists(metaFile))
                {
                    needWrite = true;
                }
                else
                {
                    var timeDiff = File.GetLastWriteTime(metaFile) - File.GetLastWriteTime(item);
                    if(Math.Abs(timeDiff.Seconds) > 5)
                    {
                        needWrite = true;
                    }
                }
                if (needWrite || scanOnly)
                {
                    try
                    {
                        TryGetInformation(item, out Image img, out string title, out string subtitle, out double replayGain);
                        if (img != null)
                        {
                            setImage(img);
                        }
                        lblMusicInfo.Text = $"标题：{title}\r\n副标题：{subtitle}\r\n回放增益：{replayGain}";
                        btnDoScan.Text = $"扫描中：{current}/{total}";
                        Application.DoEvents();
                        if (!scanOnly)
                        {
                            SaveMetaFile(metaFile, img, title, subtitle, replayGain);
                            File.SetLastWriteTime(metaFile, File.GetLastWriteTime(item));
                        }
                    }
                    catch (Exception ex)
                    {
                        print(item);
                        print(ex.ToString());
                    }
                    
                }

                
            }

            lblMusicInfo.Text = "扫描完成";
            btnDoScan.Text = "开始扫描";
            print("========= 扫描完成========= ");
            Application.DoEvents();

            btnDoScan.Enabled = true;
        }

        private void setImage(Image img)
        {
            var oldImg = imgPreview.Image;
            imgPreview.Image = img;
            oldImg?.Dispose();
        }

        private void print(string str)
        {
            textBox1.AppendText(str + "\r\n");
            textBox1.SelectionStart = textBox1.Text.Length;
            textBox1.ScrollToCaret();
        }

        private void TryGetInformation(string filePath, out Image img, out string title, out string subtitle, out double replayGain)
        {
            var tag = Un4seen.Bass.AddOn.Tags.BassTags.BASS_TAG_GetFromFile(filePath);
            if (tag == null)
            {
                print("[WARN] 无ID3Tag："+filePath);
                img = null;
                title = Path.GetFileNameWithoutExtension(filePath);
                subtitle = "Various Artist";
                replayGain = -6;
                return;
            }
            var replaygain = tag.replaygain_track_gain;
            var replaypeak = tag.replaygain_track_peak;
            if(replaygain > -101 && replaygain < -99)
            {
                print("[WARN] 无回放增益：" + filePath);
                replaygain = -6;
            }
            replayGain = replaygain;

            title = Path.GetFileNameWithoutExtension(filePath);
            if (!string.IsNullOrWhiteSpace(tag.title))
            {
                title = tag.title;
            }
            bool isAlbumNull = string.IsNullOrWhiteSpace(tag.album);
            bool isArtistNull = string.IsNullOrWhiteSpace(tag.artist);
            if (isAlbumNull && isArtistNull)
            {
                subtitle = "Various Artist";
            }
            else
            {
                if(!(isAlbumNull || isArtistNull))
                {
                    subtitle = tag.artist + " - " + tag.album;
                }
                else
                {
                    subtitle = isAlbumNull ? tag.album : tag.artist;
                }
            }
            img = null;
            if (tag.PictureCount > 0)
            {
                try
                {
                    TagPicture tagp = null;
                    for (int i = 0; i < 32; i++)
                    {
                        tagp = tag.PictureGet(i);
                        if (tagp != null) { break; }
                    }

                    var pic = tagp.PictureImage;
                    if (pic != null)
                    {
                        img = pic;
                    }

                }
                catch (Exception ex)
                {
                    print(filePath);
                    print("读取Tag失败：" + ex.ToString());
                    img = null;
                }
            }
            else
            {
                print("[WARN] 无封面图：" + filePath);
                img = null;
            }
        }

        private void SaveMetaFile(string outFile,Image img,string title,string subtitle,double replayGain)
        {
            var parentDir = Path.GetDirectoryName(outFile);
            if (!Directory.Exists(parentDir))
            {
                Directory.CreateDirectory(parentDir);
            }

            


            using(FileStream fs = File.Create(outFile))
            {
                using (BinaryWriter bw = new BinaryWriter(fs))
                {
                    byte[] titleBuf = Encoding.UTF8.GetBytes(title);
                    byte[] subtitleBuf = Encoding.UTF8.GetBytes(subtitle);
                    byte[] imgBuf = null;
                    bw.Write(titleBuf.Length);
                    bw.Write(titleBuf);
                    bw.Write(subtitleBuf.Length);
                    bw.Write(subtitleBuf);
                    double db2Linear = Decibels.DecibelsToLinear(replayGain);
                    var level =(uint) Math.Round(db2Linear * 65536);
                    bw.Write(level);
                    if (img != null)
                    {
                        using (MemoryStream imgMs = new MemoryStream())
                        {
                            try
                            {
                                using (Bitmap bmp = new Bitmap(460, 460))
                                {
                                    using (Graphics g = Graphics.FromImage(bmp))
                                    {
                                        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
                                        g.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
                                        g.Clear(Color.Black);
                                        var imgOrigionalSize = img.Size;
                                        if (Math.Abs(img.Size.Width - img.Size.Height) > 10)
                                        {
                                            var scale = (float)bmp.Width / (float)Math.Max(img.Size.Width, img.Size.Height);
                                            var renderSize = new Size((int)Math.Round(img.Size.Width * scale), (int)Math.Round(img.Size.Height * scale));
                                            g.DrawImage(img, bmp.Width / 2 - renderSize.Width / 2, bmp.Height / 2 - renderSize.Height / 2, renderSize.Width, renderSize.Height);
                                        }
                                        else
                                        {
                                            g.DrawImage(img, 0, 0, bmp.Width, bmp.Height);
                                        }
                                    }
                                    bmp.Save(imgMs, ImageFormat.Jpeg);
                                }
                                imgBuf = imgMs.ToArray();
                            }
                            catch (Exception ex)
                            {
                                print("[WARN] 图片解码失败：" + outFile);
                            }
                        }
                    }
                    if(imgBuf != null)
                    {
                        bw.Write(imgBuf.Length);
                        bw.Write(imgBuf);
                    }
                    else
                    {
                        bw.Write(0);
                    }

                }
            }


        }
        class Decibels
        {
            // 20 / ln( 10 )
            private const double LOG_2_DB = 8.6858896380650365530225783783321;

            // ln( 10 ) / 20
            private const double DB_2_LOG = 0.11512925464970228420089957273422;

            /// <summary>
            /// linear to dB conversion
            /// </summary>
            /// <param name="lin">linear value</param>
            /// <returns>decibel value</returns>
            public static double LinearToDecibels(double lin)
            {
                return Math.Log(lin) * LOG_2_DB;
            }

            /// <summary>
            /// dB to linear conversion
            /// </summary>
            /// <param name="dB">decibel value</param>
            /// <returns>linear value</returns>
            public static double DecibelsToLinear(double dB)
            {
                return Math.Exp(dB * DB_2_LOG);
            }

        }
        private void Form1_Load(object sender, EventArgs e)
        {

        }

        private void Form1_FormClosing(object sender, FormClosingEventArgs e)
        {
            e.Cancel = !btnDoScan.Enabled;
        }

        private void btnClear_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            textBox1.Clear();
        }

        private void 写入WiFi对时信息ToolStripMenuItem_Click(object sender, EventArgs e)
        {
            new FrmSetWifiConfig().ShowDialog(this);
        }
    }
}

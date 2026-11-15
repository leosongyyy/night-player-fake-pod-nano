using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace TestMP3
{
    public partial class FrmSetWifiConfig : Form
    {
        public FrmSetWifiConfig()
        {
            InitializeComponent();
            try
            {
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            }
            catch { Icon = SystemIcons.Application; }
        }

        private void FrmSetWifiConfig_Load(object sender, EventArgs e)
        {
            loadData();
        }

        string ConfigFile { get { return Path.Combine(Program.RootDir, ".ecov", "wifi.txt"); } }

        public void loadData()
        {
            if (File.Exists(ConfigFile))
            {
                var lines = File.ReadAllLines(ConfigFile);
                if (lines.Length >= 1)
                {
                    txtWifiName.Text = lines[0];
                }
                if (lines.Length >= 2)
                {
                    txtWiFiPassword.Text = lines[1];
                }
                if (lines.Length >= 3)
                {
                    txtNTPServer.Text = lines[2];
                }
            }
        }
        public bool saveData()
        {
            var wifiName = txtWifiName.Text;
            var wifiPassword = txtWiFiPassword.Text;
            var ntpServer = txtNTPServer.Text;

            // WiFi名称不能为空
            if (string.IsNullOrWhiteSpace(wifiName))
            {
                MessageBox.Show(this, "请输入WiFi名称");
                return false;
            }

            // WiFi名称不得超过32位
            if (wifiName.Length > 32)
            {
                MessageBox.Show(this, "WiFi名称不得超过32位");
                return false;
            }

            // WiFi密码不能为空
            if (string.IsNullOrWhiteSpace(wifiPassword))
            {
                MessageBox.Show(this, "请输入WiFi密码");
                return false;
            }

            // WiFi密码长度必须在8-64位之间
            if (wifiPassword.Length < 8 || wifiPassword.Length > 64)
            {
                MessageBox.Show(this, "WiFi密码长度必须在8-64位之间");
                return false;
            }

            // NTP服务器可为空，但长度不能超过64位
            if (!string.IsNullOrWhiteSpace(ntpServer) && ntpServer.Length > 64)
            {
                MessageBox.Show(this, "NTP服务器地址长度不得超过64位");
                return false;
            }

            // 按 name-pwd-ntp 拼接为数组
            string[] finalData;
            if (string.IsNullOrWhiteSpace(ntpServer))
            {
                finalData = new string[]
                {
                    wifiName,
                    wifiPassword
                };
            }
            else
            {
                finalData = new string[]
                {
                    wifiName,
                    wifiPassword,
                    ntpServer
                };
            }
            if (!Directory.Exists(Path.GetDirectoryName(ConfigFile)))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(ConfigFile));
            }
            File.WriteAllLines(ConfigFile, finalData);
            MessageBox.Show(this, "保存成功");
            return true;
        }

        private void btnSave_Click(object sender, EventArgs e)
        {
            if (saveData())
            {
                Close();
            }
        }
    }
}

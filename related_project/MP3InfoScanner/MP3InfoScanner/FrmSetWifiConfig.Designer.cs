namespace TestMP3
{
    partial class FrmSetWifiConfig
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.btnSave = new System.Windows.Forms.Button();
            this.label1 = new System.Windows.Forms.Label();
            this.label2 = new System.Windows.Forms.Label();
            this.label3 = new System.Windows.Forms.Label();
            this.label4 = new System.Windows.Forms.Label();
            this.txtWifiName = new System.Windows.Forms.TextBox();
            this.txtWiFiPassword = new System.Windows.Forms.TextBox();
            this.txtNTPServer = new System.Windows.Forms.TextBox();
            this.groupBox1 = new System.Windows.Forms.GroupBox();
            this.groupBox1.SuspendLayout();
            this.SuspendLayout();
            // 
            // btnSave
            // 
            this.btnSave.Location = new System.Drawing.Point(145, 160);
            this.btnSave.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.btnSave.Name = "btnSave";
            this.btnSave.Size = new System.Drawing.Size(120, 31);
            this.btnSave.TabIndex = 0;
            this.btnSave.Text = "保存";
            this.btnSave.UseVisualStyleBackColor = true;
            this.btnSave.Click += new System.EventHandler(this.btnSave_Click);
            // 
            // label1
            // 
            this.label1.Location = new System.Drawing.Point(41, 36);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(98, 22);
            this.label1.TabIndex = 1;
            this.label1.Text = "WiFi名称：";
            this.label1.TextAlign = System.Drawing.ContentAlignment.MiddleRight;
            // 
            // label2
            // 
            this.label2.Location = new System.Drawing.Point(41, 76);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(98, 22);
            this.label2.TabIndex = 1;
            this.label2.Text = "WiFi密码：";
            this.label2.TextAlign = System.Drawing.ContentAlignment.MiddleRight;
            // 
            // label3
            // 
            this.label3.Location = new System.Drawing.Point(41, 117);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(98, 22);
            this.label3.TabIndex = 1;
            this.label3.Text = "时间服务器：";
            this.label3.TextAlign = System.Drawing.ContentAlignment.MiddleRight;
            // 
            // label4
            // 
            this.label4.Dock = System.Windows.Forms.DockStyle.Fill;
            this.label4.Location = new System.Drawing.Point(3, 22);
            this.label4.Name = "label4";
            this.label4.Size = new System.Drawing.Size(413, 206);
            this.label4.TabIndex = 2;
            this.label4.Text = "1. 信息保存在SD卡根目录的 .ecov/wifi.txt中\r\n2. 时间服务器不填默认ntp.aliyun.com\r\n3. 仅支持带密码的2.4G WiFi\r" +
    "\n4. 密码明文保存在存储卡中。WiFi密码不建议设置得和其它密码一样。若丢失设备建议及时修改所有密码，或者用手机给播放器开临时热点+简单密码以避免泄漏";
            // 
            // txtWifiName
            // 
            this.txtWifiName.Location = new System.Drawing.Point(145, 34);
            this.txtWifiName.Name = "txtWifiName";
            this.txtWifiName.Size = new System.Drawing.Size(245, 26);
            this.txtWifiName.TabIndex = 3;
            // 
            // txtWiFiPassword
            // 
            this.txtWiFiPassword.Location = new System.Drawing.Point(145, 74);
            this.txtWiFiPassword.Name = "txtWiFiPassword";
            this.txtWiFiPassword.Size = new System.Drawing.Size(245, 26);
            this.txtWiFiPassword.TabIndex = 4;
            // 
            // txtNTPServer
            // 
            this.txtNTPServer.Location = new System.Drawing.Point(145, 115);
            this.txtNTPServer.Name = "txtNTPServer";
            this.txtNTPServer.Size = new System.Drawing.Size(245, 26);
            this.txtNTPServer.TabIndex = 5;
            // 
            // groupBox1
            // 
            this.groupBox1.Controls.Add(this.label4);
            this.groupBox1.Location = new System.Drawing.Point(12, 214);
            this.groupBox1.Name = "groupBox1";
            this.groupBox1.Size = new System.Drawing.Size(419, 231);
            this.groupBox1.TabIndex = 6;
            this.groupBox1.TabStop = false;
            this.groupBox1.Text = "说明";
            // 
            // FrmSetWifiConfig
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(8F, 20F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(445, 457);
            this.Controls.Add(this.groupBox1);
            this.Controls.Add(this.txtNTPServer);
            this.Controls.Add(this.txtWiFiPassword);
            this.Controls.Add(this.txtWifiName);
            this.Controls.Add(this.label3);
            this.Controls.Add(this.label2);
            this.Controls.Add(this.label1);
            this.Controls.Add(this.btnSave);
            this.Font = new System.Drawing.Font("微软雅黑", 10.5F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point, ((byte)(134)));
            this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedSingle;
            this.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.MaximizeBox = false;
            this.MinimizeBox = false;
            this.Name = "FrmSetWifiConfig";
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterParent;
            this.Text = "设置WiFi对时参数";
            this.Load += new System.EventHandler(this.FrmSetWifiConfig_Load);
            this.groupBox1.ResumeLayout(false);
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.Button btnSave;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.Label label3;
        private System.Windows.Forms.Label label4;
        private System.Windows.Forms.TextBox txtWifiName;
        private System.Windows.Forms.TextBox txtWiFiPassword;
        private System.Windows.Forms.TextBox txtNTPServer;
        private System.Windows.Forms.GroupBox groupBox1;
    }
}
namespace TestMP3
{
    partial class Form1
    {
        /// <summary>
        /// 必需的设计器变量。
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// 清理所有正在使用的资源。
        /// </summary>
        /// <param name="disposing">如果应释放托管资源，为 true；否则为 false。</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows 窗体设计器生成的代码

        /// <summary>
        /// 设计器支持所需的方法 - 不要修改
        /// 使用代码编辑器修改此方法的内容。
        /// </summary>
        private void InitializeComponent()
        {
            this.components = new System.ComponentModel.Container();
            this.imgPreview = new System.Windows.Forms.PictureBox();
            this.btnDoScan = new System.Windows.Forms.Button();
            this.textBox1 = new System.Windows.Forms.TextBox();
            this.groupBox1 = new System.Windows.Forms.GroupBox();
            this.groupBox2 = new System.Windows.Forms.GroupBox();
            this.btnClear = new System.Windows.Forms.LinkLabel();
            this.groupBox3 = new System.Windows.Forms.GroupBox();
            this.lblMusicInfo = new System.Windows.Forms.Label();
            this.panel1 = new System.Windows.Forms.Panel();
            this.chkScanOnly = new System.Windows.Forms.RadioButton();
            this.chkScanAndSave = new System.Windows.Forms.RadioButton();
            this.toolTip1 = new System.Windows.Forms.ToolTip(this.components);
            this.menuStrip1 = new System.Windows.Forms.MenuStrip();
            this.播放器管理ToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.写入WiFi对时信息ToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            ((System.ComponentModel.ISupportInitialize)(this.imgPreview)).BeginInit();
            this.groupBox1.SuspendLayout();
            this.groupBox2.SuspendLayout();
            this.groupBox3.SuspendLayout();
            this.panel1.SuspendLayout();
            this.menuStrip1.SuspendLayout();
            this.SuspendLayout();
            // 
            // imgPreview
            // 
            this.imgPreview.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.imgPreview.Dock = System.Windows.Forms.DockStyle.Fill;
            this.imgPreview.Location = new System.Drawing.Point(8, 29);
            this.imgPreview.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.imgPreview.Name = "imgPreview";
            this.imgPreview.Size = new System.Drawing.Size(266, 266);
            this.imgPreview.SizeMode = System.Windows.Forms.PictureBoxSizeMode.Zoom;
            this.imgPreview.TabIndex = 0;
            this.imgPreview.TabStop = false;
            // 
            // btnDoScan
            // 
            this.btnDoScan.Location = new System.Drawing.Point(13, 529);
            this.btnDoScan.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.btnDoScan.Name = "btnDoScan";
            this.btnDoScan.Size = new System.Drawing.Size(282, 48);
            this.btnDoScan.TabIndex = 1;
            this.btnDoScan.Text = "开始扫描";
            this.btnDoScan.UseVisualStyleBackColor = true;
            this.btnDoScan.Click += new System.EventHandler(this.btnDoScan_Click);
            // 
            // textBox1
            // 
            this.textBox1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.textBox1.Location = new System.Drawing.Point(8, 29);
            this.textBox1.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.textBox1.Multiline = true;
            this.textBox1.Name = "textBox1";
            this.textBox1.ScrollBars = System.Windows.Forms.ScrollBars.Vertical;
            this.textBox1.Size = new System.Drawing.Size(562, 507);
            this.textBox1.TabIndex = 2;
            // 
            // groupBox1
            // 
            this.groupBox1.Controls.Add(this.imgPreview);
            this.groupBox1.Location = new System.Drawing.Point(13, 31);
            this.groupBox1.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.groupBox1.Name = "groupBox1";
            this.groupBox1.Padding = new System.Windows.Forms.Padding(8, 10, 8, 10);
            this.groupBox1.Size = new System.Drawing.Size(282, 305);
            this.groupBox1.TabIndex = 4;
            this.groupBox1.TabStop = false;
            this.groupBox1.Text = "封面";
            // 
            // groupBox2
            // 
            this.groupBox2.Controls.Add(this.btnClear);
            this.groupBox2.Controls.Add(this.textBox1);
            this.groupBox2.Location = new System.Drawing.Point(303, 31);
            this.groupBox2.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.groupBox2.Name = "groupBox2";
            this.groupBox2.Padding = new System.Windows.Forms.Padding(8, 10, 8, 10);
            this.groupBox2.Size = new System.Drawing.Size(578, 546);
            this.groupBox2.TabIndex = 5;
            this.groupBox2.TabStop = false;
            this.groupBox2.Text = "日志输出";
            // 
            // btnClear
            // 
            this.btnClear.AutoSize = true;
            this.btnClear.Location = new System.Drawing.Point(69, -1);
            this.btnClear.Name = "btnClear";
            this.btnClear.Size = new System.Drawing.Size(37, 20);
            this.btnClear.TabIndex = 3;
            this.btnClear.TabStop = true;
            this.btnClear.Text = "清空";
            this.btnClear.LinkClicked += new System.Windows.Forms.LinkLabelLinkClickedEventHandler(this.btnClear_LinkClicked);
            // 
            // groupBox3
            // 
            this.groupBox3.Controls.Add(this.lblMusicInfo);
            this.groupBox3.Location = new System.Drawing.Point(13, 346);
            this.groupBox3.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.groupBox3.Name = "groupBox3";
            this.groupBox3.Padding = new System.Windows.Forms.Padding(8, 10, 8, 10);
            this.groupBox3.Size = new System.Drawing.Size(282, 146);
            this.groupBox3.TabIndex = 6;
            this.groupBox3.TabStop = false;
            this.groupBox3.Text = "信息";
            // 
            // lblMusicInfo
            // 
            this.lblMusicInfo.Dock = System.Windows.Forms.DockStyle.Fill;
            this.lblMusicInfo.Location = new System.Drawing.Point(8, 29);
            this.lblMusicInfo.Margin = new System.Windows.Forms.Padding(4, 0, 4, 0);
            this.lblMusicInfo.Name = "lblMusicInfo";
            this.lblMusicInfo.Size = new System.Drawing.Size(266, 107);
            this.lblMusicInfo.TabIndex = 0;
            this.lblMusicInfo.Text = "歌曲信息";
            // 
            // panel1
            // 
            this.panel1.Controls.Add(this.chkScanOnly);
            this.panel1.Controls.Add(this.chkScanAndSave);
            this.panel1.Location = new System.Drawing.Point(13, 500);
            this.panel1.Name = "panel1";
            this.panel1.Size = new System.Drawing.Size(282, 21);
            this.panel1.TabIndex = 7;
            // 
            // chkScanOnly
            // 
            this.chkScanOnly.AutoSize = true;
            this.chkScanOnly.Location = new System.Drawing.Point(106, 0);
            this.chkScanOnly.Name = "chkScanOnly";
            this.chkScanOnly.Size = new System.Drawing.Size(97, 24);
            this.chkScanOnly.TabIndex = 1;
            this.chkScanOnly.Text = "仅测试标签";
            this.chkScanOnly.UseVisualStyleBackColor = true;
            // 
            // chkScanAndSave
            // 
            this.chkScanAndSave.AutoSize = true;
            this.chkScanAndSave.Checked = true;
            this.chkScanAndSave.Location = new System.Drawing.Point(3, 0);
            this.chkScanAndSave.Name = "chkScanAndSave";
            this.chkScanAndSave.Size = new System.Drawing.Size(97, 24);
            this.chkScanAndSave.TabIndex = 0;
            this.chkScanAndSave.TabStop = true;
            this.chkScanAndSave.Text = "扫描并保存";
            this.chkScanAndSave.UseVisualStyleBackColor = true;
            // 
            // menuStrip1
            // 
            this.menuStrip1.Items.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.播放器管理ToolStripMenuItem});
            this.menuStrip1.Location = new System.Drawing.Point(0, 0);
            this.menuStrip1.Name = "menuStrip1";
            this.menuStrip1.Size = new System.Drawing.Size(891, 25);
            this.menuStrip1.TabIndex = 8;
            this.menuStrip1.Text = "menuStrip1";
            // 
            // 播放器管理ToolStripMenuItem
            // 
            this.播放器管理ToolStripMenuItem.DropDownItems.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.写入WiFi对时信息ToolStripMenuItem});
            this.播放器管理ToolStripMenuItem.Name = "播放器管理ToolStripMenuItem";
            this.播放器管理ToolStripMenuItem.Size = new System.Drawing.Size(80, 21);
            this.播放器管理ToolStripMenuItem.Text = "播放器管理";
            // 
            // 写入WiFi对时信息ToolStripMenuItem
            // 
            this.写入WiFi对时信息ToolStripMenuItem.Name = "写入WiFi对时信息ToolStripMenuItem";
            this.写入WiFi对时信息ToolStripMenuItem.Size = new System.Drawing.Size(180, 22);
            this.写入WiFi对时信息ToolStripMenuItem.Text = "写入WiFi对时信息";
            this.写入WiFi对时信息ToolStripMenuItem.Click += new System.EventHandler(this.写入WiFi对时信息ToolStripMenuItem_Click);
            // 
            // Form1
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(8F, 20F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(891, 590);
            this.Controls.Add(this.panel1);
            this.Controls.Add(this.groupBox3);
            this.Controls.Add(this.groupBox2);
            this.Controls.Add(this.groupBox1);
            this.Controls.Add(this.btnDoScan);
            this.Controls.Add(this.menuStrip1);
            this.Font = new System.Drawing.Font("微软雅黑", 10.5F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point, ((byte)(134)));
            this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedSingle;
            this.MainMenuStrip = this.menuStrip1;
            this.Margin = new System.Windows.Forms.Padding(4, 5, 4, 5);
            this.MaximizeBox = false;
            this.Name = "Form1";
            this.Text = "FakePodNano × NightPlayer - 封面扫描工具";
            this.FormClosing += new System.Windows.Forms.FormClosingEventHandler(this.Form1_FormClosing);
            this.Load += new System.EventHandler(this.Form1_Load);
            ((System.ComponentModel.ISupportInitialize)(this.imgPreview)).EndInit();
            this.groupBox1.ResumeLayout(false);
            this.groupBox2.ResumeLayout(false);
            this.groupBox2.PerformLayout();
            this.groupBox3.ResumeLayout(false);
            this.panel1.ResumeLayout(false);
            this.panel1.PerformLayout();
            this.menuStrip1.ResumeLayout(false);
            this.menuStrip1.PerformLayout();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.PictureBox imgPreview;
        private System.Windows.Forms.Button btnDoScan;
        private System.Windows.Forms.TextBox textBox1;
        private System.Windows.Forms.GroupBox groupBox1;
        private System.Windows.Forms.GroupBox groupBox2;
        private System.Windows.Forms.GroupBox groupBox3;
        private System.Windows.Forms.Label lblMusicInfo;
        private System.Windows.Forms.Panel panel1;
        private System.Windows.Forms.RadioButton chkScanOnly;
        private System.Windows.Forms.RadioButton chkScanAndSave;
        private System.Windows.Forms.LinkLabel btnClear;
        private System.Windows.Forms.ToolTip toolTip1;
        private System.Windows.Forms.MenuStrip menuStrip1;
        private System.Windows.Forms.ToolStripMenuItem 播放器管理ToolStripMenuItem;
        private System.Windows.Forms.ToolStripMenuItem 写入WiFi对时信息ToolStripMenuItem;
    }
}


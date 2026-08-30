# NightPlayer - FakePodNano版

将NightPlayer的优秀体验带到FakePodNano上

## 开发环境：

Deepseek Harness + ESP IDF 5.5.4/5.5.5

## 构建方式：

1. 安装 ESP-IDF 5.5.4 或者 5.5.5版本，安装到默认路径（C:/esp）
2. 双击 StartIDFCServer.vbs 启动 IDF Server
3. 启动 Deepseek Harness，将当前文件夹作为工作区打开
4. 在聊天框输入 `帮我构建这个项目`，发送

## 刷入方式：

1. FakePodNano 开机，插上电脑。
2. 打开设备管理器，找到串行端口一栏
3. 插拔设备，确认是哪个COM口
4. 打开 Deepseek Harness，在聊天框输入 `帮我烧录到 COMx` （这里的COMx替换成实际的COM口），发送



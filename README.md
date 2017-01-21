# qga-bc

## Information （说明）

1. This is qemu-guest-agent for BCLinux's images, it uesd in the openstack env, can monitor the system and do sometingelse.

2. It based on the [github:qemu](https://github.com/qemu/qemu/tree/stable-2.6/qga)  project

## First to know （请先读我）

> 代码主要分三种：**qemu-master**版、**qemu-kvm-0.12.1.2**版、**qemu-windows**版，具体如下
> linux代码主要修改了两个文件：commands-posix.c和qapi-schema.json（qemu-kvm-0.12.1.2是qapi-guest-schema.json），
> windows代码主要修改的是commands-win32.c和qapi-schema.json。添加的代码都用##########包起来了

1. qemu-master版适用于centos7、ubuntu14.04、ubuntu12.04；

2. qemu-kvm-0.12.1.2版适用于centos6.5、centos6.4；

3. qemu-windows版适用于windows-server2012。

## How to compile qga （如何编译）

> 64位和32位的qga需要分别在64位的机器和32位的机器上编译。

### 1.qemu-master(以在ubuntu14.04上编译64位的qga为例)：

- 准备编译环境：

> 备注：添加新功能的话，主要修改如下两个文件：commands-posix.c，qapi-schema.json

    sudo apt-get install git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev git-email libaio-dev libbluetooth-dev libbrlapi-dev libbz2-dev libcap-dev libcap-ng-dev libcurl4-gnutls-dev libgtk-3-dev libibverbs-dev libjpeg8-dev libncurses5-dev libnuma-dev librbd-dev librdmacm-dev libsasl2-dev libsdl1.2-dev libseccomp-dev libsnappy-dev libssh2-1-dev libvde-dev libvdeplug-dev libvte-2.90-dev libxen-dev liblzo2-dev valgrind xfslibs-dev


- 编译：

    cd /root/qemu-master
    mkdir -p bin/debug/native
    cd /root/qemu-master/bin/debug/native
    ../../../configure --enable-debug
    make qemu-ga

### 2.qemu-kvm-0.12.1.2(以在centos6.5上编译64位的qga为例)：

- 准备编译环境：

> 备注：添加新功能的话，主要修改如下两个文件：commands-posix.c，qapi-guest-schema.json

    此处略过

- 编译：

    cd /root/rpmbuild/BUILD/qemu-kvm-0.12.1.2
    mkdir -p bin/debug/native
    cd bin/debug/native
    ../../../configure
    make qemu-ga

### 3.qemu-windows(在docker中编译)：

- 安装docker：

    centos6.5:yum -y install docker-io
    ubuntu14.04:yum -y install docker.io

- 编译：

    docker run --rm --volume=/root/qemu-2.5.0/:/tmp/qemu-2.5.0 -t -i e3rp4y/centos-mingw-base
    
    yum -y install libgsf wget
    
    wget ftp://195.220.108.108/linux/fedora/linux/releases/21/Everything/x86_64/os/Packages/l/libgcab1-0.4-6.fc21.x86_64.rpm
    
    scp 10.128.3.75:~/docker-lib/msitools-0.95-1.fc24.x86_64.rpm root@computer:~/qemu-2.5.0/
    scp 10.128.3.75:~/docker-lib/libmsi1-0.95-1.fc24.x86_64.rpm root@computer:~/qemu-2.5.0/
    rpm -ivh libgcab1-0.4-6.fc21.x86_64.rpm	

    cd /tmp/qemu-2.5.0

    rpm -ivh libmsi1-0.95-1.fc24.x86_64.rpm
    rpm -ivh msitools-0.95-1.fc24.x86_64.rpm

    mkdir build
    cd build
    ../configure --target-list=x86_64-softmmu --cross-prefix=x86_64-w64-mingw32- --enable-guest-agent --enable-guest-agent-msi
    make V=1 VL_LDFLAGS=-Wl,--build-id qemu-ga-x86_64.msi

- 生成msi文件即可在windows上安装。


## The interaction protocol （交互协议的设计） 

### 内存监控

    输入：{"execute":"guest-get-memory-status"}
    输出：{"return": {"total":xx,"used":xxx,"buffer":xx,,"cached":xx,"swap":{"total":xx,"used":xxx}}} 备注：这里单位默认为MB
    例：virsh qemu-agent-command centos7 '{"execute":"guest-get-memory-status"}'
 

### 系统信息监控
    输入：{"execute":"guest-get-system-info"}
    输出：{"return": {"os-name":"xxxx","kernel-version":”xxxx”,"system-version":”xxxx”,"fqdn":”xxxx”,"lastlogin":"xxxx"}} 
    例：virsh qemu-agent-command centos7 '{"execute":"guest-get-system-info"}'
 
### oom状态监控
    输入：{"execute":"guest-get-oom-status"}
    输出：{"return": {"oom-happened":"true/false"}} 
    例：virsh qemu-agent-command centos7 '{"execute":"guest-get-oom-status"}'
 
### 应用程序监控
    输入：{"execute":"guest-get-app-status"}
    输出：{"return": {"appStatus":"xxx"}} 
    备注："xxx"为top的result
    例：virsh qemu-agent-command centos7 '{"execute":"guest-get-app-status"}'
 
### 磁盘监控
    输入：{"execute":"guest-get-disk-status"}
    输出：{"return": {"挂载点1":{"total":xxx1,"used",yyy1,"writable",true/false},"挂载点2":{"total":xxx2,"used",yyy2,"writable",true/false}}}
    例：virsh qemu-agent-command centos7 '{"execute":"guest-get-disk-status"}'
 
### 用户自定义监控

    输入：{"execute":"guest-user-check","arguments":{"command-name":"command-xxxx}} 
    输出：{"return": {"command-name":"resultxxx"}}
    备注：command-name由用户定义，resultxxx为执行command-xxx的返回值)
    例：virsh qemu-agent-command centos7 '{"execute":"guest-user-check", "arguments": {"command-name":"list", "command":"ls -l"}}'
 
### 修改主机名

    输入：{"execute":" change-hostname”,"arguments":{"new-hostname":"xxxx”}}’
    输出：{"return":{"errnum":X}}
    备注：new-hostname由用户定义，errnum为执行修改主机名的返回值)
    例：virsh qemu-agent-command centos6.5 '{"execute":"change-hostname", "arguments": {"new-hostname":"name1"}}'
 
###修改root密码

    输入：{"execute":" change-password”,"arguments":{"new-password":"xxxx”}}’
    输出：{"return":{"errnum":X}}
    备注：new-password由用户定义，errnum为执行修改root密码的返回值)
    例：virsh qemu-agent-command centos6.5 '{"execute":"change-password", "arguments": {"new-password":"123456"}}'
 

## Monitoring Items （监控项） 

    memory.total 内存总量
    memory.used 内存已使用
    memory.buffer 内存buffer量
    memory.cached 内存cached量
    memory.swap.total 交换空间总量
    memory.swap.used 交换空间已使用量
    instance.system.info 操作系统信息，包括操作系统类型，版本，最后登录时间等
    instance.oom.status 操作系统内存溢出状态
    instance.app.status 操作系统应用程序状态
    disk.total 磁盘挂载点的总量
    disk.used 磁盘挂载点的已使用量
    disk.writable 磁盘挂载点是否可读写
    user.check 用户自定义检查


## End of file.

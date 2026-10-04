// SPDX-License-Identifier: GPL-3.0-or-later
#include "drawing_enrollment.hpp"
#include "client_enrollment.h"
#include "noise.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <sodium.h>
#include <thread>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static std::uint64_t ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::uint8_t command(PltrDrawingEnrollment &g, const std::string &name,
    unsigned action, const std::uint8_t id[16], const std::uint8_t client[32], const std::uint8_t target[32]) {
    const int fd = socket(AF_UNIX,SOCK_STREAM,0); assert(fd>=0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    memcpy(address.sun_path+1,name.data(),name.size());
    assert(connect(fd,reinterpret_cast<sockaddr *>(&address),offsetof(sockaddr_un,sun_path)+1+name.size())==0);
    std::uint8_t request[86], reply[22]; memcpy(request,"PLEN\1",5); request[5]=action;
    memcpy(request+6,id,16); memcpy(request+22,client,32); memcpy(request+54,target,32);
    assert(send(fd,request,86,MSG_NOSIGNAL)==86);
    for (int i=0;i<4;++i) g.service(ms());
    assert(recv(fd,reply,22,MSG_WAITALL)==22); close(fd);
    assert(memcmp(reply,"PLEN\1",5)==0 && memcmp(reply+6,id,16)==0); return reply[5];
}
static int proof(PltrDrawingEnrollment &grants, PltrIdentityStore &store,
    const std::uint8_t private_key[32], const std::uint8_t target[32], const std::uint8_t id[16], bool confirm=true) {
    int pair[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    int server_result=99;
    std::thread server([&]{server_result=pltr_run_enrollment(pair[0],store,grants,-1); close(pair[0]);});
    auto *client=pltr_client_enrollment_create(private_key,target,id); assert(client);
    std::uint8_t out[256],in[64]; std::size_t n=0, consumed=0,written=0;
    assert(pltr_client_enrollment_start(client,out,sizeof(out),&n)==0);
    assert(send(pair[1],out,n,MSG_NOSIGNAL)==static_cast<ssize_t>(n));
    int result=-1;
    while (true) {
        const auto count=recv(pair[1],in,sizeof(in),0);
        if (count<=0) break;
        std::size_t offset=0;
        while (offset<static_cast<std::size_t>(count)) {
            result=pltr_client_enrollment_receive(client,in+offset,count-offset,&consumed,out,sizeof(out),&written);
            assert(consumed>0); offset+=consumed;
            if (result<0 || result==2 || (written && !confirm)) break;
            if (written) assert(send(pair[1],out,written,MSG_NOSIGNAL)==static_cast<ssize_t>(written));
        }
        if (result<0 || result==2 || (written && !confirm)) break;
    }
    close(pair[1]); server.join(); pltr_client_enrollment_destroy(client);
    if (result==2) assert(server_result==0); else assert(server_result==-1);
    return result;
}
int main() {
    assert(sodium_init()>=0);
    char dir[]="/tmp/plank-enrollment-test-XXXXXX"; assert(mkdtemp(dir));
    assert(chmod(dir,0700)==0);
    PltrIdentityStore store{}; assert(pltr_identity_store_open(&store,dir)==0);
    std::uint8_t private_key[32], public_key[32], wrong_private[32], wrong_public[32],id[16];
    randombytes_buf(private_key,32); randombytes_buf(wrong_private,32);
    assert(pltr_noise_public_key(private_key,public_key)==0);
    assert(pltr_noise_public_key(wrong_private,wrong_public)==0);
    const std::string name="plank-enrollment-test-"+std::to_string(getpid());
    {
        // A substitute test uid is explicit and confined to this listener.
        // A caller with our uid must not reach mutation under another policy.
        PltrDrawingEnrollment denied(store,{name+"-denied",static_cast<std::uint32_t>(getuid()+1)});
        assert(denied.bind());
        int fd=socket(AF_UNIX,SOCK_STREAM,0); assert(fd>=0);
        sockaddr_un a{}; a.sun_family=AF_UNIX;
        const auto denied_name=name+"-denied";
        memcpy(a.sun_path+1,denied_name.data(),denied_name.size());
        assert(connect(fd,reinterpret_cast<sockaddr *>(&a),offsetof(sockaddr_un,sun_path)+1+denied_name.size())==0);
        denied.service(ms());
        char byte; assert(recv(fd,&byte,1,0)==0); close(fd);
    }
    {
        PltrDrawingEnrollment grants(store,{name,static_cast<std::uint32_t>(getuid())}); assert(grants.bind());
        randombytes_buf(id,16);
        assert(command(grants,name,1,id,public_key,wrong_public)==2);
        assert(!grants.claim(id,public_key,ms()));
        assert(command(grants,name,1,id,public_key,store.public_key)==0);
        assert(command(grants,name,1,id,public_key,store.public_key)==3);
        assert(proof(grants,store,wrong_private,store.public_key,id)==-1);
        assert(!pltr_identity_store_approve(&store,public_key));
        assert(proof(grants,store,private_key,store.public_key,id)==2);
        assert(pltr_identity_store_approve(&store,public_key));
        assert(proof(grants,store,private_key,store.public_key,id)==-1); // consumed proof
        assert(pltr_identity_store_remove(&store,public_key)==0);
        randombytes_buf(id,16);
        assert(command(grants,name,1,id,public_key,store.public_key)==0);
        assert(proof(grants,store,private_key,store.public_key,id,false)==1); // cancel before confirm
        assert(!pltr_identity_store_approve(&store,public_key));
        assert(proof(grants,store,private_key,store.public_key,id)==-1);
        randombytes_buf(id,16);
        assert(command(grants,name,1,id,public_key,store.public_key)==0);
        assert(command(grants,name,2,id,public_key,store.public_key)==0);
        assert(proof(grants,store,private_key,store.public_key,id)==-1);
        randombytes_buf(id,16);
        assert(command(grants,name,1,id,public_key,store.public_key)==0);
        assert(!grants.claim(id,public_key,ms()+PltrDrawingEnrollment::GrantLifetimeMs));
        assert(!pltr_identity_store_approve(&store,public_key));
        randombytes_buf(id,16);
        assert(command(grants,name,1,id,public_key,store.public_key)==0);
        assert(proof(grants,store,private_key,wrong_public,id)==-1); // wrong drawing server
        assert(!pltr_identity_store_approve(&store,public_key));
    }
    pltr_identity_store_close(&store);
    for (const auto *file:{"identity.key","paired-clients.json","store.lock"}) {
        const auto path=std::string(dir)+"/"+file; unlink(path.c_str());
    }
    // Exact owned temporary path, no recursive deletion or production names.
    assert(rmdir(dir)==0);
    return 0;
}

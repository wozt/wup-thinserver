#define _GNU_SOURCE
#include "kms_wire.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
/* Private inherited seqpacket socket and caller-opened primary DRM node.
 * No filesystem paths, shell commands, arbitrary framebuffer IDs or DRM writes. */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts(KMS_HELPER_VERSION);
        return 0;
    }
    if (argc != 1) return 1;
    const int sock=3, card=4;
    int type=0; socklen_t len=sizeof type;
    if (getuid()==0 || getsockopt(sock,SOL_SOCKET,SO_TYPE,&type,&len) ||
        type!=SOCK_SEQPACKET || drmGetNodeTypeFromFd(card)!=DRM_NODE_PRIMARY) return 1;
    struct ucred peer; len=sizeof peer;
    if (getsockopt(sock,SOL_SOCKET,SO_PEERCRED,&peer,&len) || peer.uid!=getuid()) return 1;
    for (;;) {
        struct kms_request q;
        ssize_t n=recv(sock,&q,sizeof q,MSG_TRUNC);
        if(n==0) return 0;
        if(n!=(ssize_t)sizeof q || q.magic!=KMS_WIRE_MAGIC) return 1;
        struct kms_reply r={.magic=KMS_WIRE_MAGIC};
        int fds[4]={-1,-1,-1,-1};
        drmModeCrtc *c=drmModeGetCrtc(card,q.crtc);
        drmModeFB2 *fb=NULL;
        if(c && c->mode_valid && c->buffer_id) {
            r.x=c->x; r.y=c->y; r.sw=c->width; r.sh=c->height;
            fb=drmModeGetFB2(card,c->buffer_id);
        }
        if(c) drmModeFreeCrtc(c);
        if(fb) {
            r.id=fb->fb_id; r.width=fb->width; r.height=fb->height;
            r.format=fb->pixel_format; r.flags=fb->flags; r.modifier=fb->modifier;
            if(r.format==DRM_FORMAT_XRGB8888 || r.format==DRM_FORMAT_ARGB8888) {
                for(int i=0;i<4;i++) {
                    if(!fb->handles[i]) continue;
                    if((unsigned)i!=r.count || !fb->pitches[i] ||
                       drmPrimeHandleToFD(card,fb->handles[i],DRM_CLOEXEC|DRM_RDWR,&fds[i])) {r.count=0;break;}
                    r.pitch[i]=fb->pitches[i];r.offset[i]=fb->offsets[i];r.count++;
                }
            }
            for(int i=0;i<4;i++) if(fb->handles[i]) {
                int duplicate=0;
                for(int j=0;j<i;j++) if(fb->handles[j]==fb->handles[i]) duplicate=1;
                if(!duplicate) {struct drm_gem_close g={0};g.handle=fb->handles[i];drmIoctl(card,DRM_IOCTL_GEM_CLOSE,&g);}
            }
            drmModeFreeFB2(fb);
        }
        if(r.count>1 && !(r.flags&DRM_MODE_FB_MODIFIERS)) r.count=0;
        union {struct cmsghdr align; char bytes[CMSG_SPACE(4*sizeof(int))];} control={0};
        struct iovec iov={&r,sizeof r}; struct msghdr msg={0};msg.msg_iov=&iov;msg.msg_iovlen=1;
        if(r.count) {
            msg.msg_control=control.bytes;msg.msg_controllen=CMSG_SPACE(r.count*sizeof(int));
            struct cmsghdr *h=CMSG_FIRSTHDR(&msg);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;
            h->cmsg_len=CMSG_LEN(r.count*sizeof(int));memcpy(CMSG_DATA(h),fds,r.count*sizeof(int));
        }
        n=sendmsg(sock,&msg,MSG_NOSIGNAL);
        for(int i=0;i<4;i++) if(fds[i]>=0) close(fds[i]);
        if(n!=(ssize_t)sizeof r) return 1;
    }
}

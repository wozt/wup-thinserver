#define _GNU_SOURCE
#include "capture_kms.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <unistd.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
#include "kms_wire.h"
#define FW 854
#define FH 480
struct wup_kms {
    int fd, sw, sh, x, y, w, h, gpu;
    uint32_t crtc;
    int helper_sock;
    pid_t helper_pid;
    int cpu_render_fd;
    int fb_x, fb_y;
    int screen_x, screen_y;
    struct gbm_device *gbm;
    struct gbm_bo *bo;
    void *map;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program, texture, fbo, color;
    GLuint yuv_program, yuv_fbo, yuv_color;
    int render_fd;
    struct gbm_device *render_gbm;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image;
};
static int fail(const char *why) { fprintf(stderr,"[capture] KMS: %s (no fallback)\n",why); return -1; }
/* WUP_KMS_MULTI_MONITOR: match exact DRM names or AMD XRandR zero-based names. */
static int connector_matches(const char *xname, const char *drmname) {
    if (!strcmp(xname, drmname)) return 1;
    unsigned index; char tail; char candidate[64];
    if (sscanf(xname,"DisplayPort-%u%c",&index,&tail)==1 && index<1000) {
        snprintf(candidate,sizeof candidate,"DP-%u",index+1);
        return !strcmp(candidate,drmname);
    }
    if (sscanf(xname,"HDMI-A-%u%c",&index,&tail)==1 && index<1000) {
        snprintf(candidate,sizeof candidate,"HDMI-A-%u",index+1);
        return !strcmp(candidate,drmname);
    }
    return 0;
}
/* WUP_KMS_HELPER_0888: one helper per capture, private inherited socket. */
static void close_primes(int prime[4]) {
    for(int i=0;i<4;i++) if(prime[i]>=0) {close(prime[i]);prime[i]=-1;}
}
static int helper_start(wup_kms_t *k) {
    int sp[2];
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sp)) return -1;
    int s=fcntl(sp[1],F_DUPFD_CLOEXEC,10), d=fcntl(k->fd,F_DUPFD_CLOEXEC,10);
    close(sp[1]);
    if(s<0 || d<0) {if(s>=0)close(s);if(d>=0)close(d);close(sp[0]);return -1;}
    posix_spawn_file_actions_t fa;
    int rc=posix_spawn_file_actions_init(&fa);
    if(!rc) {
        rc=posix_spawn_file_actions_adddup2(&fa,s,3);
        if(!rc)rc=posix_spawn_file_actions_adddup2(&fa,d,4);
        char *argv[]={KMS_HELPER_PATH,NULL}; char *env[]={"PATH=/usr/bin:/bin",NULL};
        if(!rc)rc=posix_spawn(&k->helper_pid,KMS_HELPER_PATH,&fa,NULL,argv,env);
        posix_spawn_file_actions_destroy(&fa);
    }
    close(s);close(d);
    if(rc) {close(sp[0]);k->helper_pid=0;fprintf(stderr,"[capture] KMS helper: %s\n",strerror(rc));return -1;}
    k->helper_sock=sp[0];
    struct timeval timeout={2,0};
    if(setsockopt(sp[0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout) ||
       setsockopt(sp[0],SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout)) return -1;
    return 0;
}
static int export_frame(wup_kms_t *k,drmModeFB2 **fb,int prime[4],int *planes) {
    *fb=NULL;*planes=0;for(int i=0;i<4;i++)prime[i]=-1;
    struct kms_request q={KMS_WIRE_MAGIC,k->crtc};
    if(send(k->helper_sock,&q,sizeof q,MSG_NOSIGNAL)!=(ssize_t)sizeof q) return fail("helper request failed");
    struct kms_reply r={0};struct iovec iov={&r,sizeof r};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(4*sizeof(int))];} control={0};
    struct msghdr msg={0};msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control.bytes;msg.msg_controllen=sizeof control.bytes;
    ssize_t n=recvmsg(k->helper_sock,&msg,MSG_CMSG_CLOEXEC);
    int count=0;
    if(n>=0) for(struct cmsghdr *h=CMSG_FIRSTHDR(&msg);h;h=CMSG_NXTHDR(&msg,h)) {
        if(h->cmsg_level!=SOL_SOCKET || h->cmsg_type!=SCM_RIGHTS || h->cmsg_len<CMSG_LEN(0)) continue;
        size_t nf=(h->cmsg_len-CMSG_LEN(0))/sizeof(int);
        for(size_t i=0;i<nf;i++){int fd;memcpy(&fd,(char*)CMSG_DATA(h)+i*sizeof fd,sizeof fd);if(count<4)prime[count++]=fd;else close(fd);}
    }
    if(n!=(ssize_t)sizeof r || (msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || r.magic!=KMS_WIRE_MAGIC ||
       !r.count || r.count!=(uint32_t)count || r.sw!=(uint32_t)k->sw || r.sh!=(uint32_t)k->sh ||
       (uint64_t)r.x+k->x+k->w>r.width || (uint64_t)r.y+k->y+k->h>r.height ||
       (r.format!=DRM_FORMAT_XRGB8888 && r.format!=DRM_FORMAT_ARGB8888)) {
        close_primes(prime);return fail("helper returned no usable framebuffer (check installed capability and display mode)");
    }
    *fb=calloc(1,sizeof **fb);if(!*fb){close_primes(prime);return -1;}
    (*fb)->fb_id=r.id;(*fb)->width=r.width;(*fb)->height=r.height;(*fb)->pixel_format=r.format;
    (*fb)->flags=r.flags;(*fb)->modifier=r.modifier;
    memcpy((*fb)->pitches,r.pitch,sizeof r.pitch);memcpy((*fb)->offsets,r.offset,sizeof r.offset);
    k->fb_x=(int)r.x+k->x;k->fb_y=(int)r.y+k->y;*planes=count;return 0;
}
static GLuint shader(GLenum kind,const char *source) {
    GLuint s=glCreateShader(kind); glShaderSource(s,1,&source,NULL); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]={0}; glGetShaderInfoLog(s,sizeof log,NULL,log); fprintf(stderr,"KMS shader: %s\n",log); glDeleteShader(s); return 0; }
    return s;
}
/* Packed I420: 854*480*3/2 bytes = 427*360 RGBA pixels.
 * The first pass preserves the existing BGRA scaling and letterbox exactly.
 * The second pass reproduces libwupstream's integer BT.601 conversion.
 * Fragment highp is required for exact linear byte indices. */
int wup_kms_i420_enabled(void) {
    const char *v = getenv("WUP_KMS_RGB");
    return !v || strcmp(v, "1");
}
static int yuv_setup(wup_kms_t *k) {
    static const char vs[] = "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.,1.);}";
    static const char fs[] =
        "precision highp float; uniform sampler2D image;"
        "float sample_byte(float i){"
        "float plane=0.; float w=854.; float q=i;"
        "if(i>=409920.){plane=1.; w=427.; q=i-409920.;}"
        "if(i>=512400.){plane=2.; q=i-512400.;}"
        "float y=floor(q/w); float x=q-y*w;"
        "if(plane>0.){x*=2.;y*=2.;}"
        "vec3 rgb=floor(texture2D(image,(vec2(x,y)+0.5)/vec2(854.,480.)).bgr*255.+0.5);"
        "float n=dot(rgb,vec3(66.,129.,25.)); float bias=16.;"
        "if(plane==1.){n=dot(rgb,vec3(-38.,-74.,112.));bias=128.;}"
        "if(plane==2.){n=dot(rgb,vec3(112.,-94.,-18.));bias=128.;}"
        "return (floor((n+128.)/256.)+bias)/255.;}"
        "void main(){float i=(floor(gl_FragCoord.y)*427.+floor(gl_FragCoord.x))*4.;"
        "gl_FragColor=vec4(sample_byte(i),sample_byte(i+1.),sample_byte(i+2.),sample_byte(i+3.));}";
    GLuint v=shader(GL_VERTEX_SHADER,vs), f=shader(GL_FRAGMENT_SHADER,fs);
    GLint ok=0;
    if(v&&f){
        k->yuv_program=glCreateProgram();glAttachShader(k->yuv_program,v);glAttachShader(k->yuv_program,f);
        glBindAttribLocation(k->yuv_program,0,"pos");glLinkProgram(k->yuv_program);
        glGetProgramiv(k->yuv_program,GL_LINK_STATUS,&ok);
    }
    if(v)glDeleteShader(v);
    if(f)glDeleteShader(f);
    if(!ok)return fail("I420 shader link failed (WUP_KMS_RGB=1 restores RGB)");
    glGenTextures(1,&k->yuv_color);glBindTexture(GL_TEXTURE_2D,k->yuv_color);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,427,360,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glGenFramebuffers(1,&k->yuv_fbo);glBindFramebuffer(GL_FRAMEBUFFER,k->yuv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,k->yuv_color,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE || glGetError()!=GL_NO_ERROR)
        return fail("I420 framebuffer failed");
    fprintf(stderr,"[capture] KMS EGL packed I420 enabled (BT.601 limited)\n");
    return 0;
}
/* WUP_EGL_OFFSCREEN_0886 */
static int egl_fail(const char *stage) {
    fprintf(stderr,"[capture] KMS EGL %s: error=0x%x\n",stage,(unsigned)eglGetError());
    return -1;
}
static int egl_setup(wup_kms_t *k) {
    char *node=drmGetRenderDeviceNameFromFd(k->fd);
    if (!node) return fail("no render node for selected DRM device");
    k->render_fd=open(node,O_RDWR|O_CLOEXEC);
    fprintf(stderr,"[capture] KMS EGL render node %s: %s\n",node,k->render_fd<0?strerror(errno):"opened");
    free(node);
    if (k->render_fd<0) return -1;
    k->render_gbm=gbm_create_device(k->render_fd);
    if (!k->render_gbm) return fail("render-node GBM creation failed");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) return egl_fail("get platform display entrypoint");
    k->display=get_display(EGL_PLATFORM_GBM_KHR,k->render_gbm,NULL);
    if (k->display==EGL_NO_DISPLAY) return egl_fail("get display");
    EGLint major=0,minor=0;
    if (!eglInitialize(k->display,&major,&minor)) return egl_fail("initialize");
    fprintf(stderr,"[capture] KMS EGL %d.%d vendor=%s\n",major,minor,eglQueryString(k->display,EGL_VENDOR));
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return egl_fail("bind GLES");
    const char *ext=eglQueryString(k->display,EGL_EXTENSIONS);
    if (!ext || !strstr(ext,"EGL_EXT_image_dma_buf_import") || !strstr(ext,"EGL_KHR_surfaceless_context"))
        return fail("EGL DMA-BUF import or surfaceless context unavailable");
    EGLint cfgattr[]={EGL_SURFACE_TYPE,0,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig cfg; EGLint n=0;
    if (!eglChooseConfig(k->display,cfgattr,&cfg,1,&n)) return egl_fail("choose config");
    if (!n) return fail("no GLES2 EGL config");
    EGLint ctxattr[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    k->context=eglCreateContext(k->display,cfg,EGL_NO_CONTEXT,ctxattr);
    if (k->context==EGL_NO_CONTEXT) return egl_fail("create context");
    if (!eglMakeCurrent(k->display,EGL_NO_SURFACE,EGL_NO_SURFACE,k->context)) return egl_fail("make surfaceless current");
    fprintf(stderr,"[capture] KMS EGL renderer=%s\n",(const char *)glGetString(GL_RENDERER));
    k->create_image=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    k->destroy_image=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    k->bind_image=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    GLuint v=shader(GL_VERTEX_SHADER,"attribute vec2 pos; attribute vec2 uv; varying vec2 tex; void main(){gl_Position=vec4(pos,0.,1.);tex=uv;}");
    GLuint f=shader(GL_FRAGMENT_SHADER,"precision mediump float; varying vec2 tex; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,tex).bgra;}");
    GLint ok=0;
    if (v && f && k->create_image && k->destroy_image && k->bind_image) {
        k->program=glCreateProgram(); glAttachShader(k->program,v); glAttachShader(k->program,f);
        glBindAttribLocation(k->program,0,"pos"); glBindAttribLocation(k->program,1,"uv"); glLinkProgram(k->program);
        glGetProgramiv(k->program,GL_LINK_STATUS,&ok);
        if (!ok) { char log[512]={0}; glGetProgramInfoLog(k->program,sizeof log,NULL,log); fprintf(stderr,"[capture] KMS EGL link: %s\n",log); }
        glGenTextures(1,&k->texture);
        glGenTextures(1,&k->color); glBindTexture(GL_TEXTURE_2D,k->color);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,FW,FH,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glGenFramebuffers(1,&k->fbo); glBindFramebuffer(GL_FRAMEBUFFER,k->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,k->color,0);
        GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER), error=glGetError();
        if (status!=GL_FRAMEBUFFER_COMPLETE || error!=GL_NO_ERROR) {
            fprintf(stderr,"[capture] KMS EGL FBO status=0x%x GL error=0x%x\n",(unsigned)status,(unsigned)error); ok=0;
        }
    }
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    if (ok && wup_kms_i420_enabled() && yuv_setup(k)) ok=0;
    if (!eglMakeCurrent(k->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT)) return egl_fail("release context");
    return ok?0:fail("GLES shader, import entrypoint or FBO setup failed");
}
wup_kms_t *wup_kms_open(const char *output,int sw,int sh,int sx,int sy,int x,int y,int w,int h,int gpu) {
    if(sw<=0||sh<=0||x<0||y<0||w<=0||h<=0||x>sw-w||y>sh-h) return NULL;
    wup_kms_t *k=calloc(1,sizeof *k); if(!k) return NULL;
    k->helper_sock=-1;k->cpu_render_fd=-1;k->render_fd=-1;k->fd=-1;k->sw=sw;k->sh=sh;k->x=x;k->y=y;k->w=w;k->h=h;k->gpu=gpu;
    k->screen_x=sx; k->screen_y=sy;
    glob_t paths={0};
    const char *forced=getenv("WUP_KMS_DEVICE");
    if(glob(forced&&*forced?forced:"/dev/dri/card[0-9]*",0,NULL,&paths)!=0) goto bad;
    for(size_t i=0;i<paths.gl_pathc;i++) {
        int fd=open(paths.gl_pathv[i],O_RDWR|O_CLOEXEC); if(fd<0) { fprintf(stderr,"[capture] KMS open %s: %s\n",paths.gl_pathv[i],strerror(errno)); continue; }
        drmModeRes *r=drmModeGetResources(fd);
        if(r) {
            for(int j=0;j<r->count_connectors;j++) {
                drmModeConnector *c=drmModeGetConnector(fd,r->connectors[j]); if(!c) continue;
                char name[64]; const char *type=drmModeGetConnectorTypeName(c->connector_type);
                snprintf(name,sizeof name,"%s-%u",type?type:"unknown",c->connector_type_id);
                if(c->connection==DRM_MODE_CONNECTED && c->encoder_id && connector_matches(output,name)) {
                    drmModeEncoder *e=drmModeGetEncoder(fd,c->encoder_id);
                    if(e&&e->crtc_id) {
                        drmModeCrtc *cr=drmModeGetCrtc(fd,e->crtc_id);
                        if(cr&&cr->mode_valid&&cr->width==(uint32_t)sw&&cr->height==(uint32_t)sh) {
                            fprintf(stderr,"[capture] KMS candidate %s -> %s CRTC %u origin %u,%u (X11 %d,%d)\n",output,name,e->crtc_id,cr->x,cr->y,sx,sy);
                            /* An individual scanout may have origin zero. A shared scanout must match XRandR. */
                            if ((cr->x==0 && cr->y==0) || (sx>=0 && sy>=0 && cr->x==(uint32_t)sx && cr->y==(uint32_t)sy)) { k->fd=fd;k->crtc=e->crtc_id; }
                        }
                        if(cr) drmModeFreeCrtc(cr);
                    }
                    if(e) drmModeFreeEncoder(e);
                }
                drmModeFreeConnector(c); if(k->fd>=0) break;
            }
            drmModeFreeResources(r);
        }
        if(k->fd>=0) { fprintf(stderr,"[capture] KMS %s: %s connector %s CRTC %u\n",gpu?"EGL":"CPU",paths.gl_pathv[i],output,k->crtc); break; }
        close(fd);
    }
    globfree(&paths);
    if(k->fd<0) goto bad;
    if(helper_start(k)) goto bad;
    if(!gpu) {
        char *node=drmGetRenderDeviceNameFromFd(k->fd);
        if(!node) goto bad;
        k->cpu_render_fd=open(node,O_RDWR|O_CLOEXEC);free(node);
        if(k->cpu_render_fd<0) goto bad;
        k->gbm=gbm_create_device(k->cpu_render_fd);if(!k->gbm) goto bad;
    }
    drmModeFB2 *fb=NULL; int prime[4], planes;
    if(export_frame(k,&fb,prime,&planes)) goto bad;
    fprintf(stderr,"[capture] KMS framebuffer %u: %ux%u crop=%d,%d %dx%d modifier=%llx\n",fb->fb_id,fb->width,fb->height,k->fb_x,k->fb_y,k->w,k->h,(unsigned long long)fb->modifier);
    fprintf(stderr,"[capture] KMS exported %d DMA-BUF plane(s)\n",planes);
    for (int i=0; i<planes; i++)
        fprintf(stderr,"[capture] KMS plane %d pitch=%u offset=%u\n",i,fb->pitches[i],fb->offsets[i]);
    close_primes(prime); drmModeFreeFB2(fb);
    if(gpu&&egl_setup(k)) { fail("EGL initialization failed"); goto bad; }
    fprintf(stderr,"[capture] DRM/KMS %s selected (no fallback)\n",gpu?"EGL":"CPU");
    return k;
bad:
    fail("initialization failed; verify connector name, permissions and primary scanout"); wup_kms_close(k); return NULL;
}
void wup_kms_release(wup_kms_t *k) {
    if(!k||!k->bo) return;
    if(k->map) gbm_bo_unmap(k->bo,k->map);
    gbm_bo_destroy(k->bo);k->bo=NULL;k->map=NULL;
}
int wup_kms_cpu(wup_kms_t *k,const uint8_t **pixels,int *stride) {
    if(!k||k->gpu) return -1;
    wup_kms_release(k);
    drmModeFB2 *fb=NULL;int prime[4], planes;
    if(export_frame(k,&fb,prime,&planes)) return -1;
    if(fb->flags & DRM_MODE_FB_MODIFIERS) {
        struct gbm_import_fd_modifier_data d={0};d.width=fb->width;d.height=fb->height;d.format=fb->pixel_format;d.num_fds=(uint32_t)planes;d.modifier=fb->modifier;
        for (int i=0; i<planes; i++) {
            d.fds[i]=prime[i]; d.strides[i]=fb->pitches[i]; d.offsets[i]=fb->offsets[i];
        }
        k->bo=gbm_bo_import(k->gbm,GBM_BO_IMPORT_FD_MODIFIER,&d,0);
    } else if(planes==1 && !fb->offsets[0]) {
        struct gbm_import_fd_data d={0};d.fd=prime[0];d.width=fb->width;d.height=fb->height;d.stride=fb->pitches[0];d.format=fb->pixel_format;
        k->bo=gbm_bo_import(k->gbm,GBM_BO_IMPORT_FD,&d,0);
    }
    close_primes(prime);drmModeFreeFB2(fb);
    if(!k->bo) {
        fprintf(stderr,"[capture] KMS GBM import (%d planes): %s\n",planes,strerror(errno));
        return fail("GBM import failed");
    }
    uint32_t pitch=0;
    void *ptr=gbm_bo_map(k->bo,k->fb_x,k->fb_y,k->w,k->h,GBM_BO_TRANSFER_READ,&pitch,&k->map);
    if(!ptr||pitch<(uint32_t)k->w*4||pitch>INT_MAX) {wup_kms_release(k);return fail("GBM CPU mapping failed");}
    *pixels=ptr;*stride=(int)pitch;return 0;
}
int wup_kms_egl(wup_kms_t *k,unsigned char *out,int x,int y,int w,int h) {
    if(!k||!k->gpu) return -1;
    drmModeFB2 *fb=NULL;int prime[4], planes;
    if(export_frame(k,&fb,prime,&planes)) return -1;
    if(!eglMakeCurrent(k->display,EGL_NO_SURFACE,EGL_NO_SURFACE,k->context)) {close_primes(prime);drmModeFreeFB2(fb);return egl_fail("make current for frame");}
    glBindFramebuffer(GL_FRAMEBUFFER,k->fbo);
    EGLint attrs[48]={EGL_WIDTH,(EGLint)fb->width,EGL_HEIGHT,(EGLint)fb->height,
        EGL_LINUX_DRM_FOURCC_EXT,(EGLint)fb->pixel_format};
    int n=6, ret=-1;
    static const EGLint fd_key[4]={EGL_DMA_BUF_PLANE0_FD_EXT,EGL_DMA_BUF_PLANE1_FD_EXT,EGL_DMA_BUF_PLANE2_FD_EXT,EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint offset_key[4]={EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGL_DMA_BUF_PLANE1_OFFSET_EXT,EGL_DMA_BUF_PLANE2_OFFSET_EXT,EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint pitch_key[4]={EGL_DMA_BUF_PLANE0_PITCH_EXT,EGL_DMA_BUF_PLANE1_PITCH_EXT,EGL_DMA_BUF_PLANE2_PITCH_EXT,EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint lo_key[4]={EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint hi_key[4]={EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    int modified=(fb->flags & DRM_MODE_FB_MODIFIERS)!=0;
    if (modified) {
        const char *ext=eglQueryString(k->display,EGL_EXTENSIONS);
        if (!ext || !strstr(ext,"EGL_EXT_image_dma_buf_import_modifiers")) goto done;
    }
    for (int i=0; i<planes; i++) {
        if (fb->offsets[i]>INT_MAX || fb->pitches[i]>INT_MAX) goto done;
        attrs[n++]=fd_key[i]; attrs[n++]=prime[i];
        attrs[n++]=offset_key[i]; attrs[n++]=(EGLint)fb->offsets[i];
        attrs[n++]=pitch_key[i]; attrs[n++]=(EGLint)fb->pitches[i];
        if (modified) {
            attrs[n++]=lo_key[i]; attrs[n++]=(EGLint)(uint32_t)fb->modifier;
            attrs[n++]=hi_key[i]; attrs[n++]=(EGLint)(uint32_t)(fb->modifier>>32);
        }
    }
    attrs[n]=EGL_NONE;
    EGLImageKHR image=k->create_image(k->display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,attrs);
    if(image==EGL_NO_IMAGE_KHR) {
        fprintf(stderr,"[capture] KMS EGL import error 0x%x (planes=%d)\n",(unsigned)eglGetError(),planes);
        goto done;
    }
    glBindTexture(GL_TEXTURE_2D,k->texture);k->bind_image(GL_TEXTURE_2D,(GLeglImageOES)image);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glViewport(0,0,FW,FH);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glViewport(x,y,w,h);glUseProgram(k->program);
    float l=(float)k->fb_x/fb->width,r=(float)(k->fb_x+k->w)/fb->width,t=(float)k->fb_y/fb->height,b=(float)(k->fb_y+k->h)/fb->height;
    const GLfloat pos[]={-1,-1,1,-1,-1,1,1,1},uv[]={l,t,r,t,l,b,r,b};
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,pos);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,0,uv);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    if(k->yuv_program) {
        glBindFramebuffer(GL_FRAMEBUFFER,k->yuv_fbo);
        glViewport(0,0,427,360);glUseProgram(k->yuv_program);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,k->color);
        glDisable(GL_DITHER);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,pos);
        glDisableVertexAttribArray(1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        glReadPixels(0,0,427,360,GL_RGBA,GL_UNSIGNED_BYTE,out);
        glEnable(GL_DITHER);
    } else glReadPixels(0,0,FW,FH,GL_RGBA,GL_UNSIGNED_BYTE,out);
    ret=glGetError()==GL_NO_ERROR?0:-1;
    /* Drop the imported scanout reference before next frame. */
    glDeleteTextures(1,&k->texture);glGenTextures(1,&k->texture);
    k->destroy_image(k->display,image);
done:
    close_primes(prime);drmModeFreeFB2(fb);
    eglMakeCurrent(k->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    return ret?fail("EGL import/draw/readback failed"):0;
}
void wup_kms_close(wup_kms_t *k) {
    if(!k)return;
    wup_kms_release(k);
    if(k->display!=EGL_NO_DISPLAY) {
        if(k->context!=EGL_NO_CONTEXT&&eglMakeCurrent(k->display,EGL_NO_SURFACE,EGL_NO_SURFACE,k->context)) {
            if(k->texture)glDeleteTextures(1,&k->texture);
            if(k->fbo)glDeleteFramebuffers(1,&k->fbo);
            if(k->color)glDeleteTextures(1,&k->color);
            if(k->program)glDeleteProgram(k->program);
            if(k->yuv_program)glDeleteProgram(k->yuv_program);
            if(k->yuv_fbo)glDeleteFramebuffers(1,&k->yuv_fbo);
            if(k->yuv_color)glDeleteTextures(1,&k->yuv_color);
            eglMakeCurrent(k->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        }
        if(k->surface!=EGL_NO_SURFACE)eglDestroySurface(k->display,k->surface);
        if(k->context!=EGL_NO_CONTEXT)eglDestroyContext(k->display,k->context);
        eglTerminate(k->display);
    }
    if(k->render_gbm)gbm_device_destroy(k->render_gbm);
    if(k->render_fd>=0)close(k->render_fd);
    if(k->gbm)gbm_device_destroy(k->gbm);
    if(k->cpu_render_fd>=0)close(k->cpu_render_fd);
    if(k->helper_sock>=0)close(k->helper_sock);
    if(k->helper_pid>0) {
        kill(k->helper_pid,SIGTERM);
        while(waitpid(k->helper_pid,NULL,0)<0 && errno==EINTR) {}
    }
    if(k->fd>=0)close(k->fd);
    free(k);
}

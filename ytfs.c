#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	Qdir = 0,
	Qurl = 1,
};

typedef struct Stream Stream;
struct Stream{
	int pid;
	char *url;
	char *name;
	int data[2];
	Channel *cpid;
};

enum {
	Maxstream = 1024,
};

static Stream streams[Maxstream];
static int nstream = 0;

static char *remote = nil;
static char *prefix = "https://youtube.com/watch?v=";
static char *user = nil;

static char *Ebadqid = "Bad QID";
static char *Esshfail = "Ssh failed to start";
static char *Efilenotexist = "File does not exist";
static char *Ebadwalk = "Walk in non dir";
static char *Etoomany = "Too many files";

static void
sshproc(void *arg)
{
	char buf[512];
	Stream *a;

	a = arg;
	dup(a->data[0], 1);
	close(a->data[0]);
	close(a->data[1]);
	close(2);
	snprint(buf, sizeof buf, "youtube-dl --youtube-skip-dash-manifest -o - -f bestaudio %s | "
		"ffmpeg -y -i - -f mp3 -b:a 160k -metadata title=\"`youtube-dl -e %s`\" -metadata album=ytfs -",
		a->url, a->url);
	procexecl(a->cpid, "/bin/ssh", "ssh", remote, "bash", "-c", buf, nil);
}

static void
fsattach(Req *r)
{
	r->fid->qid = (Qid){Qdir, 0, QTDIR};
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	int i, c;

	switch((ulong)fid->qid.path) {
	case Qdir:
		if(strcmp(name, "..") == 0) {
			*qid = (Qid){Qdir, 0, QTDIR};
			fid->qid = *qid;
			return nil;
		}
		for(c=0,i=0;c<nstream && i<Maxstream;i++){
			if(streams[i].url == nil){
				continue;
			}
			c++;
			if(strcmp(name, streams[i].name) == 0){
				*qid = (Qid){Qurl+i, 0, QTFILE};
				fid->qid = *qid;
				return nil;	
			}
		}
		return Efilenotexist;
	default:
		return Ebadwalk;
	}
}

static void
fscreate(Req *r)
{
	int i;
	char *name;
	Stream *s;

	name = r->ifcall.name;
	for(i=0, s=streams; i<Maxstream && s->url!=nil; i++, s++)
		;
	if(i == Maxstream){
		respond(r, Etoomany);
		return;
	}
	s->url = smprint("%s%s", prefix, name);
	s->name = smprint("%s%s", name, ".mp3");
	r->fid->qid = (Qid){Qurl+i, 0, 0};
	r->ofcall.qid = r->fid->qid;
	nstream++;
	respond(r, nil);
}

static void
fsremove(Req *r)
{
	long path;

	path = r->fid->qid.path;
	path -= Qurl;
	if(path < 0){
		respond(r, Ebadqid);
		return;
	}
	free(streams[path].url);
	free(streams[path].name);
	streams[path].url = nil;
	streams[path].name = nil;
	nstream--;
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	Fid *fid;
	Stream *s;
	long path;

	fid = r->fid;
	path = fid->qid.path;
	r->ofcall.qid = (Qid){fid->qid.path, 0, fid->qid.vers};
	if(fid->qid.path == Qdir){
		respond(r, nil);
		return;
	}
	path -= Qurl;
	if(path < 0){
		respond(r, Ebadqid);
		return;
	}
	s = streams+path;
	pipe(s->data);
	s->cpid = chancreate(sizeof(int), 0);
	procrfork(sshproc, s, 8192, RFFDG);
	close(s->data[0]);
	recv(s->cpid, &s->pid);
	if(s->pid < 0)
		respond(r, Esshfail);
	respond(r, nil);
}

static int
urldirgen(int n, Dir *dir, void*)
{
	Stream *s;

	nulldir(dir);
	if(n >= nstream)
		return -1;
	s = streams+n;
	dir->qid = (Qid){Qurl+n, 0, QTFILE};
	dir->mode = 0644;
	dir->name = estrdup9p(s->name);
	dir->uid = nil;
	dir->gid = nil;
	dir->muid = nil;
	return 0;
}

static void
fsread(Req *r)
{
	Stream *s;
	Fid *fid;
	long path;

	fid = r->fid;
	path = fid->qid.path;
	if(path == Qdir){
		dirread9p(r, urldirgen, nil);
		respond(r, nil);
		return;
	}
	path -= Qurl;
	if(path < 0){
		respond(r, Ebadqid);
		return;
	}
	s = streams+path;
	r->ofcall.count = read(s->data[1], r->ofcall.data, r->ifcall.count);
	respond(r, nil);
}

static void
fsstat(Req *r)
{
	Dir *d;
	Stream *s;
	long path;

	d = &r->d;
	nulldir(d);
	path = r->fid->qid.path;
	switch(path){
	case Qdir:
		d->name = estrdup9p("/");
		d->qid.type = QTDIR;
		d->mode = DMDIR|0777;
		break;
	default:
		path -= Qurl;
		if(path < 0){
			respond(r, Ebadqid);
			return;
		}
		s = streams+path;
		d->name = estrdup9p(s->name);
		d->mode = 0644;
		break;
	}
	d->uid = nil;
	d->gid = nil;
	d->muid = nil;
	respond(r, nil);
}

static void
fsdestroy(Fid *f)
{
	long path;
	Stream *s;

	path = f->qid.path;
	path -= Qurl;
	if((f->qid.type & (QTAUTH | QTDIR)) != 0 || path < 0)
		return;
	s = streams+path;
	if(s->pid != 0){
		postnote(PNGROUP, s->pid, "kill");
		close(s->data[1]);
		chanfree(s->cpid);
	}
}

Srv fs = {
.attach=	fsattach,
.walk1=		fswalk1,
.open=		fsopen,
.read=		fsread,
.create=	fscreate,
.remove=	fsremove,
.stat=		fsstat,

.destroyfid=	fsdestroy,
};

static void
usage(void)
{
	fprint(2, "Usage: %s [-m mtpt] remote\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *mtpt;

	mtpt = "/n/ytfs";
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	}ARGEND;
	if(argc < 1)
		usage();
	remote = argv[0];
	memset(streams, 0, sizeof(Stream)*Maxstream);
	threadpostmountsrv(&fs, nil,  mtpt, MREPL|MCREATE);
	threadexits(nil);
}

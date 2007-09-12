#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <ixp.h>

#define MYNAME		"ixp"
#define MYVERSION	MYNAME " library for " LUA_VERSION " / Nov 2003"

#include "lua.h"
#include "lauxlib.h"

#define L_IXP_MT "ixp.ixp_mt"
#define L_IXP_IDIR_MT "ixp.idir_mt"
#define L_IXP_IREAD_MT "ixp.iread_mt"

#ifdef DBG
#define DBGF(fmt,args...) fprintf(stderr,fmt,##args)
#else
#define DBGF(fmt,args...) ({})
#endif

/* ------------------------------------------------------------------------
 * error helper
 */
static int pusherror(lua_State *L, const char *info)
{
	lua_pushnil(L);
	if (info==NULL) {
		lua_pushstring(L, strerror(errno));
		lua_pushnumber(L, errno);
		return 3;
	} else if (errno) {
		lua_pushfstring(L, "%s: %s", info, strerror(errno));
		lua_pushnumber(L, errno);
		return 3;
	} else {
		lua_pushfstring(L, "%s", info);
		return 2;
	}
}

/* ------------------------------------------------------------------------
 * the C representation of a ixp instance object
 */
struct ixp {
	const char *address;;
	IxpClient *client;
};

static struct ixp *checkixp (lua_State *L, int narg)
{
	void *ud = luaL_checkudata (L, narg, L_IXP_MT);
	luaL_argcheck (L, ud != NULL, 1, "`ixp' expected");
	return (struct ixp*)ud;
}

static int l_ixp_tostring (lua_State *L)
{
	struct ixp *ixp = checkixp (L, 1);
	lua_pushfstring (L, "ixp instance %p", ixp);
	return 1;
}

/* ------------------------------------------------------------------------
 * lua: ixptest() 
 */
#ifdef DBG
static int l_test (lua_State *L)
{
	DBGF("** ixp.test **\n");
	return pusherror (L, "some error occurred");
}
static int l_ixp_test (lua_State *L)
{
	struct ixp *ixp = checkixp (L, 1);
	DBGF("** ixp:test (%p [%s]) **\n", ixp, ixp->address);
	return pusherror (L, "some error occurred");
}
#endif

/* ------------------------------------------------------------------------
 * lua: write(file, data) -- writes data to a file 
 */

static int write_data (IxpCFid *fid, const char *data, size_t data_len);

static int l_ixp_write (lua_State *L)
{
	struct ixp *ixp;
	IxpCFid *fid;
	const char *file;
	const char *data;
	size_t data_len;
	int rc;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);
	data = luaL_checklstring (L, 3, &data_len);

	fid = ixp_open(ixp->client, (char*)file, P9_OWRITE);
	if(fid == NULL)
		return pusherror (L, "count not open p9 file");

	DBGF("** ixp.write (%s,%s) **\n", file, data);
	
	rc = write_data (fid, data, data_len);
	if (rc < 0) {
		ixp_close(fid);
		return pusherror (L, "failed to write to p9 file");
	}

	ixp_close(fid);
	return 0;
}

static int write_data (IxpCFid *fid, const char *data, size_t data_len)
{
	size_t left;
	off_t ofs = 0;

	left = data_len;
	while (left) {
		int rc = ixp_write(fid, (char*)data + ofs, left);
		if (rc < 0)
			return rc;

		else if (rc > left)
			return -ENXIO;

		left -= rc;
		ofs += rc;
	}

	return data_len;
}


/* ------------------------------------------------------------------------
 * lua: data = read(file) -- returns all contents (upto 4k) 
 */
static int l_ixp_read (lua_State *L)
{
	struct ixp *ixp;
	IxpCFid *fid;
	const char *file;
	char *buf;
	size_t buf_ofs, buf_size, buf_left;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);

	fid = ixp_open(ixp->client, (char*)file, P9_OREAD);
	if(fid == NULL)
		return pusherror (L, "count not open p9 file");

	buf = malloc (fid->iounit);
	if (!buf) {
		ixp_close(fid);
		return pusherror (L, "count not allocate memory");
	}
	buf_ofs = 0;
	buf_size = buf_left = fid->iounit;

	DBGF("** ixp.read (%s) **\n", file);
	
	for (;;) {
		int rc = ixp_read (fid, buf+buf_ofs, buf_left);
		if (rc==0)
			break;
		else if (rc<0) {
			ixp_close(fid);
			return pusherror (L, "failed to read from p9 file");
		}

		buf_ofs += rc;
		buf_left -= rc;

		if (buf_ofs >= buf_size)
			return pusherror (L, "internal error while reading");

		if (buf_size >= 4096)
			break;

		buf = realloc (buf, 4096);
		if (!buf) {
			ixp_close(fid);
			return pusherror (L, "count not allocate memory");
		}
		buf_size = 4096;
	}

	ixp_close(fid);

	lua_pushstring (L, buf);
	return 1;
}

/* ------------------------------------------------------------------------
 * lua: create(file, [data]) -- create a file, optionally write data to it 
 */
static int l_ixp_create (lua_State *L)
{
	struct ixp *ixp;
	IxpCFid *fid;
	const char *file;
	const char *data;
	size_t data_len = 0;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);
	data = luaL_optlstring (L, 3, NULL, &data_len);

	DBGF("** ixp.create (%s) **\n", file);
	
	fid = ixp_create (ixp->client, (char*)file, 0777, P9_OWRITE);
	if (!fid)
		return pusherror (L, "count not create file");

	if (data && data_len
			&& !(fid->qid.type & P9_DMDIR)) {
		int rc = write_data (fid, data, data_len);
		if (rc < 0) {
			ixp_close(fid);
			return pusherror (L, "failed to write to p9 file");
		}
	}

	ixp_close(fid);
	return 0;
}

/* ------------------------------------------------------------------------
 * lua: remove(file) -- remove a file 
 */
static int l_ixp_remove (lua_State *L)
{
	struct ixp *ixp;
	int rc;
	const char *file;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);

	DBGF("** ixp.remove (%s) **\n", file);
	
	rc = ixp_remove (ixp->client, (char*)file);
	if (!rc)
		return pusherror (L, "failed to remove p9 file");

	return 0;
}

/* ------------------------------------------------------------------------
 * lua: itr = iread(file) -- returns a line iterator 
 */

struct l_ixp_iread_s {
	IxpCFid *fid;
	char *buf;
	size_t buf_pos;
	size_t buf_len;
	size_t buf_size;
};

static int l_ixp_iread_iter (lua_State *L);

static int l_ixp_iread (lua_State *L)
{
	struct ixp *ixp;
	const char *file;
	struct l_ixp_iread_s *ctx;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);

	ctx = (struct l_ixp_iread_s*)lua_newuserdata (L, sizeof(*ctx));
	if (!ctx)
		return pusherror (L, "count not allocate context");
	memset (ctx, 0, sizeof (*ctx));

	// set the metatable for the new userdata
	luaL_getmetatable (L, L_IXP_IREAD_MT);
	lua_setmetatable (L, -2);

	ctx->fid = ixp_open(ixp->client, (char*)file, P9_OREAD);
	if(ctx->fid == NULL) {
		return pusherror (L, "count not open p9 file");
	}

	DBGF("** ixp.iread (%s) **\n", file);

	// create and return the iterator function
	// the only argument is the userdata
	lua_pushcclosure (L, l_ixp_iread_iter, 1);
	return 1;
}

static lua_State *l_ixp_iread_lua_state = NULL;
static void l_ixp_iread_catcher (int sig)
{
	int rc, timeout;
	lua_State *L = l_ixp_iread_lua_state;

	/*
	 * attempt to call back into the lua context using the function that
	 * is passed into the iterator
	 */
	if (L) {
		timeout = luaL_optnumber (L, 1, 0);
		rc = lua_isfunction (L, 2);
		if (timeout > 0 && rc) {
			int new_timeout;

fprintf (stderr, "  **** callback\n");
			lua_call (L, 0, 1);

			new_timeout = luaL_checknumber (L, 1);
			lua_pop (L, 1);
fprintf (stderr, "  **** catcher alarm(%d)\n", new_timeout);
			alarm (new_timeout);
		}
	}
}

static int l_ixp_iread_iter (lua_State *L)
{
	struct l_ixp_iread_s *ctx;
	char *s, *e, *cr;
	int timeout;

	timeout = luaL_optnumber (L, 1, 0);
	ctx = (struct l_ixp_iread_s*)lua_touserdata (L, lua_upvalueindex(1));

	DBGF("** ixp.iread - iter **\n");
fprintf (stderr, "**** iter (%d)\n", timeout);

	if (!ctx->buf) {
		ctx->buf = malloc (ctx->fid->iounit);
		if (!ctx->buf)
			return pusherror (L, "count not allocate memory");
		ctx->buf_size = ctx->fid->iounit;
		ctx->buf_len = 0;
	}

	if (!ctx->buf_len) {
		struct sigaction sact, sact2;
		int rc;
		ctx->buf_pos = 0;

		if (timeout > 0) {
			sigemptyset (&sact.sa_mask);
			sact.sa_flags = 0;
			sact.sa_handler = l_ixp_iread_catcher;
			l_ixp_iread_lua_state = L;

			sigaction (SIGALRM, &sact, &sact2);
			alarm (timeout);
fprintf (stderr, "**** alarm (%d)\n", timeout);
		}

		rc = ixp_read (ctx->fid, ctx->buf, ctx->buf_size);

		if (timeout > 0) {
fprintf (stderr, "**** alarm (0)\n");
			alarm (0);
			l_ixp_iread_lua_state = NULL;
			sigaction (SIGALRM, &sact2, NULL);
		}

fprintf (stderr, "**** rc = %d\n", rc);
		if (rc == EINTR) {
fprintf (stderr, "**** return timeout\n");
			lua_pushstring (L, "timeout");
			return 1;
		}

		if (rc <= 0) {
fprintf (stderr, "**** return EOF\n");
			return 0; // we are done
		}
		ctx->buf_len = rc;
fprintf (stderr, "**** ...\n");
	}

	s = ctx->buf + ctx->buf_pos;
	e = s + ctx->buf_len;

	cr = strchr (s, '\n');
	if (!cr) {
		// no match, just return the whole thing
		// TODO: should read more upto a cr or some limit
		lua_pushstring (L, s);
		ctx->buf_len = 0;
		return 1;

	} else {
		// we have a match s..cr is our sub string
		int len = (cr-s) + 1;
		*cr = 0;
		lua_pushstring (L, s);
		ctx->buf_pos += len;
		ctx->buf_len -= len;
		return 1;
	}
}

static int l_ixp_iread_gc (lua_State *L)
{
	struct l_ixp_iread_s *ctx;

	ctx = (struct l_ixp_iread_s*)lua_touserdata (L, 1);

	DBGF("** ixp.iread - gc **\n");

	ixp_close (ctx->fid);

	if (ctx->buf)
		free (ctx->buf);

	return 0;
}

static void init_iread_mt (lua_State *L)
{
	luaL_newmetatable(L, L_IXP_IREAD_MT);

	// setup the __gc field
	lua_pushstring (L, "__gc");
	lua_pushcfunction (L, l_ixp_iread_gc);
	lua_settable (L, -3);
}

/* ------------------------------------------------------------------------
 * lua: stat = stat(file) -- returns a status table 
 */

static int pushstat (lua_State *L, const struct IxpStat *stat);

static int l_ixp_stat (lua_State *L)
{
	struct ixp *ixp;
	struct IxpStat *stat;
	const char *file;
	int rc;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);

	DBGF("** ixp.stat (%s) **\n", file);

	stat = ixp_stat(ixp->client, (char*)file);
	if(!stat)
		return pusherror(L, "cannot stat file");

	rc = pushstat (L, stat);

	ixp_freestat (stat);

	return rc;
}

static void setrwx(long m, char *s)
{
	static char *modes[] = {
		"---", "--x", "-w-",
		"-wx", "r--", "r-x",
		"rw-", "rwx",
	};
	strncpy(s, modes[m], 3);
}

static void build_modestr(char *buf, const struct IxpStat *stat)
{
	buf[0]='-';
	if(stat->mode & P9_DMDIR)
		buf[0]='d';
	buf[1]='-';
	setrwx((stat->mode >> 6) & 7, &buf[2]);
	setrwx((stat->mode >> 3) & 7, &buf[5]);
	setrwx((stat->mode >> 0) & 7, &buf[8]);
	buf[11] = 0;
}

static void build_timestr(char *buf, const struct IxpStat *stat)
{
	ctime_r((time_t*)&stat->mtime, buf);
	buf[strlen(buf) - 1] = '\0';
}


#define setfield(type,name,value) \
	lua_pushstring (L, name); \
	lua_push##type (L, value); \
	lua_settable (L, -3);
static int pushstat (lua_State *L, const struct IxpStat *stat)
{
	static char buf[32];
	lua_newtable (L);

	setfield(number, "type", stat->type);
	setfield(number, "dev", stat->dev);
	//setfield(Qid,    "qid", stat->qid);
	setfield(number, "mode", stat->mode);
	setfield(number, "atime", stat->atime);
	setfield(number, "mtime", stat->mtime);
	setfield(number, "length", stat->length);
	setfield(string, "name", stat->name);
	setfield(string, "uid", stat->uid);
	setfield(string, "gid", stat->gid);
	setfield(string, "muid", stat->muid);

	build_modestr(buf, stat);
	setfield(string, "modestr", buf);

	build_timestr(buf, stat);
	setfield(string, "timestr", buf);

	return 1;
}

/* ------------------------------------------------------------------------
 * lua: itr = idir(dir) -- returns a file name iterator 
 */

struct l_ixp_idir_s {
	IxpCFid *fid;
	unsigned char *buf;
	IxpMsg m;
};

static int l_ixp_idir_iter (lua_State *L);

static int l_ixp_idir (lua_State *L)
{
	struct ixp *ixp;
	const char *file;
	struct l_ixp_idir_s *ctx;

	ixp = checkixp (L, 1);
	file = luaL_checkstring (L, 2);

	ctx = (struct l_ixp_idir_s*)lua_newuserdata (L, sizeof(*ctx));
	if (!ctx)
		return pusherror (L, "count not allocate context");
	memset(ctx, 0, sizeof (*ctx));

	// set the metatable for the new userdata
	luaL_getmetatable (L, L_IXP_IDIR_MT);
	lua_setmetatable (L, -2);

	ctx->fid = ixp_open(ixp->client, (char*)file, P9_OREAD);
	if(ctx->fid == NULL) {
		return pusherror (L, "count not open p9 file");
	}

	ctx->buf = malloc (ctx->fid->iounit);
	if (!ctx->buf) {
		ixp_close (ctx->fid);
		ctx->fid = NULL;
		return pusherror (L, "count not allocate memory");
	}

	DBGF("** ixp.idir (%s) **\n", file);

	// create and return the iterator function
	// the only argument is the userdata
	lua_pushcclosure (L, l_ixp_idir_iter, 1);
	return 1;
}

static int l_ixp_idir_iter (lua_State *L)
{
	struct l_ixp_idir_s *ctx;
	IxpStat stat;

	ctx = (struct l_ixp_idir_s*)lua_touserdata (L, lua_upvalueindex(1));

	DBGF("** ixp.idir - iter **\n");

	if (ctx->m.pos >= ctx->m.end) {
		int rc = ixp_read (ctx->fid, ctx->buf, ctx->fid->iounit);
		if (rc <= 0) {
			return 0;
		}

		ctx->m = ixp_message(ctx->buf, rc, MsgUnpack);
		if (ctx->m.pos >= ctx->m.end)
			return 0;
	}

	ixp_pstat(&ctx->m, &stat);

	return pushstat (L, &stat);
}

static int l_ixp_idir_gc (lua_State *L)
{
	struct l_ixp_idir_s *ctx;

	ctx = (struct l_ixp_idir_s*)lua_touserdata (L, 1);

	DBGF("** ixp.idir - gc **\n");

	free (ctx->buf);

	ixp_close (ctx->fid);

	return 0;
}

static void init_idir_mt (lua_State *L)
{
	luaL_newmetatable(L, L_IXP_IDIR_MT);

	// setup the __gc field
	lua_pushstring (L, "__gc");
	lua_pushcfunction (L, l_ixp_idir_gc);
	lua_settable (L, -3);
}

/* ------------------------------------------------------------------------
 * lua: x = ixp.new("unix!/tmp/ns.bart.:0/wmii") -- create a new ixp object
 */
static int l_new (lua_State *L)
{
	const char *adr;
	IxpClient *cli;
	struct ixp *ixp;

	adr = luaL_checkstring (L, 1);

	DBGF("** ixp.new ([%s]) **\n", adr);

	cli = ixp_mount((char*)adr);
	if (!cli)
		return pusherror (L, "could not open ixp connection");

	ixp = (struct ixp*)lua_newuserdata(L, sizeof (struct ixp));

	luaL_getmetatable (L, L_IXP_MT);
	lua_setmetatable (L, -2);

	ixp->address = strdup (adr);
	ixp->client = cli;

	return 1;
}

static int l_ixp_gc (lua_State *L)
{
	struct ixp *ixp = checkixp (L, 1);

	DBGF("** ixp:__gc (%p [%s]) **\n", ixp, ixp->address);

	ixp_unmount (ixp->client);
	free ((char*)ixp->address);

	return 0;
}

/* ------------------------------------------------------------------------
 * the class method table 
 */
static const luaL_reg class_table[] =
{
#ifdef DBG
	{ "test",		l_test },
#endif

	{ "new",		l_new },
	
	{ NULL,			NULL },
};

/* ------------------------------------------------------------------------
 * the instance method table 
 */
static const luaL_reg instance_table[] =
{
#ifdef DBG
	{ "test",		l_ixp_test },
#endif

	{ "__tostring",		l_ixp_tostring },
	{ "__gc",		l_ixp_gc },

	{ "write",		l_ixp_write },
	{ "read",		l_ixp_read },

	{ "create",		l_ixp_create },
	{ "remove",		l_ixp_remove },

	{ "iread",		l_ixp_iread },
	{ "idir",		l_ixp_idir },

	{ "stat",		l_ixp_stat },

	{ NULL,			NULL },
};

/* ------------------------------------------------------------------------
 * the class metatable
 */
static int init_ixp_class (lua_State *L)
{
	luaL_newmetatable(L, L_IXP_MT);

	// setup the __index and __gc field
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);		// pushes the new metatable
	lua_settable (L, -3);		// metatable.__index = metatable

	luaL_openlib (L, NULL, instance_table, 0);
	luaL_openlib (L, "ixp", class_table, 0);

#if 0
	luaL_register (L, MYNAME, R);
	lua_pushliteral (L, "version");
	lua_pushliteral (L, MYVERSION);
	lua_settable (L, -3);
#endif
	return 1;
}

/* ------------------------------------------------------------------------
 * library entry
 */
LUALIB_API int luaopen_ixp (lua_State *L)
{
	init_iread_mt (L);
	init_idir_mt (L);

	return init_ixp_class (L);
}

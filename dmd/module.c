
// Compiler implementation of the D programming language
// Copyright (c) 1999-2007 by Digital Mars
// All Rights Reserved
// written by Walter Bright
// http://www.digitalmars.com
// License for redistribution is by either the Artistic License
// in artistic.txt, or the GNU General Public License in gnu.txt.
// See the included readme.txt for details.

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#if (defined (__SVR4) && defined (__sun))
#include <alloca.h>
#endif

#if _MSC_VER || __MINGW32__
#include <malloc.h>
#endif

#if IN_GCC
#include "gdc_alloca.h"
#endif

#include "mem.h"

#include "mars.h"
#include "module.h"
#include "parse.h"
#include "scope.h"
#include "identifier.h"
#include "id.h"
#include "import.h"
#include "dsymbol.h"
#include "hdrgen.h"
#include "lexer.h"

#define MARS 1
#include "html.h"

#ifdef IN_GCC
#include "d-dmd-gcc.h"
#endif

ClassDeclaration *Module::moduleinfo;

Module *Module::rootModule;
DsymbolTable *Module::modules;
Array Module::amodules;

Array Module::deferred;	// deferred Dsymbol's needing semantic() run on them
unsigned Module::dprogress;

void Module::init()
{
    modules = new DsymbolTable();
}

Module::Module(char *filename, Identifier *ident, int doDocComment, int doHdrGen)
	: Package(ident)
{
    FileName *srcfilename;

//    printf("Module::Module(filename = '%s', ident = '%s')\n", filename, ident->toChars());
    this->arg = filename;
    md = NULL;
    errors = 0;
    numlines = 0;
    members = NULL;
    isHtml = 0;
    isDocFile = 0;
    needmoduleinfo = 0;
#ifdef IN_GCC
    strictlyneedmoduleinfo = 0;
#endif
    insearch = 0;
    searchCacheIdent = NULL;
    searchCacheSymbol = NULL;
    searchCacheFlags = 0;
    semanticstarted = 0;
    semanticdone = 0;
    decldefs = NULL;
    vmoduleinfo = NULL;
    massert = NULL;
    marray = NULL;
    sictor = NULL;
    sctor = NULL;
    sdtor = NULL;
    stest = NULL;
    sfilename = NULL;
    root = 0;
    importedFrom = NULL;
    srcfile = NULL;
    objfile = NULL;
    docfile = NULL;
    hdrfile = NULL;

    debuglevel = 0;
    debugids = NULL;
    debugidsNot = NULL;
    versionlevel = 0;
    versionids = NULL;
    versionidsNot = NULL;

    macrotable = NULL;
    escapetable = NULL;
    doppelganger = 0;
    cov = NULL;
    covb = NULL;

    srcfilename = FileName::defaultExt(filename, global.mars_ext);
    if (!srcfilename->equalsExt(global.mars_ext) &&
        !srcfilename->equalsExt(global.hdr_ext) &&
        !srcfilename->equalsExt("dd"))
    {
	if (srcfilename->equalsExt("html") ||
	    srcfilename->equalsExt("htm")  ||
	    srcfilename->equalsExt("xhtml"))
	    isHtml = 1;
	else
	{   error("source file name '%s' must have .%s extension", srcfilename->toChars(), global.mars_ext);
	    fatal();
	}
    }
    srcfile = new File(srcfilename);

    // LDC
    llvmForceLogging = false;
    this->doDocComment = doDocComment;
    this->doHdrGen = doHdrGen;
}

File* Module::buildFilePath(char* forcename, char* path, char* ext)
{
    char *argobj;
    if (forcename)
	argobj = forcename;
    else
    {
	if (global.params.preservePaths)
	    argobj = (char*)this->arg;
	else
	    argobj = FileName::name((char*)this->arg);

	if (global.params.fqnNames)
	{
	    if(md)
		argobj = FileName::replaceName(argobj, md->toChars());
	    else
		argobj = FileName::replaceName(argobj, toChars());

	    // add ext, otherwise forceExt will make nested.module into nested.bc
	    size_t len = strlen(argobj);
	    size_t extlen = strlen(ext);
	    char* s = (char *)alloca(len + 1 + extlen + 1);
	    memcpy(s, argobj, len);
	    s[len] = '.';
	    memcpy(s + len + 1, ext, extlen + 1);
	    s[len+1+extlen] = 0;
	    argobj = s;
	}
    }

    if (!FileName::absolute(argobj))
    {
	argobj = FileName::combine(path, argobj);
    }

    FileName::ensurePathExists(FileName::path(argobj));

// always append the extension! otherwise hard to make output switches consistent
//    if (forcename)
//	return new File(argobj);
//    else
    // allow for .o and .obj on windows
#if _WIN32
    if (ext == global.params.objdir && FileName::ext(argobj) 
	    && stricmp(FileName::ext(argobj), global.obj_ext_alt) == 0)
	return new File(argobj);
#endif
    return new File(FileName::forceExt(argobj, ext));
}

void Module::buildTargetFiles()
{
    if(objfile && 
       (!doDocComment || docfile) && 
       (!doHdrGen || hdrfile))
	return;

    if(!objfile)
	objfile = Module::buildFilePath(global.params.objname, global.params.objdir, global.obj_ext);
    if(doDocComment && !docfile)
	docfile = Module::buildFilePath(global.params.docname, global.params.docdir, global.doc_ext);
    if(doHdrGen && !hdrfile)
	hdrfile = Module::buildFilePath(global.params.hdrname, global.params.hdrdir, global.hdr_ext);

    // safety check: never allow obj, doc or hdr file to have the source file's name
    if(stricmp(FileName::name(objfile->name->str), FileName::name((char*)this->arg)) == 0)
    {
	error("Output object files with the same name as the source file are forbidden");
	fatal();
    }
    if(docfile && stricmp(FileName::name(docfile->name->str), FileName::name((char*)this->arg)) == 0)
    {
	error("Output doc files with the same name as the source file are forbidden");
	fatal();
    }
    if(hdrfile && stricmp(FileName::name(hdrfile->name->str), FileName::name((char*)this->arg)) == 0)
    {
	error("Output header files with the same name as the source file are forbidden");
	fatal();
    }
}

void Module::deleteObjFile()
{
    if (global.params.obj)
	objfile->remove();
    //if (global.params.llvmBC)
    //bcfile->remove();
    if (doDocComment && docfile)
	docfile->remove();
}

Module::~Module()
{
}

const char *Module::kind()
{
    return "module";
}

Module *Module::load(Loc loc, Array *packages, Identifier *ident)
{   Module *m;
    char *filename;

    //printf("Module::load(ident = '%s')\n", ident->toChars());

    // Build module filename by turning:
    //	foo.bar.baz
    // into:
    //	foo\bar\baz
    filename = ident->toChars();
    if (packages && packages->dim)
    {
	OutBuffer buf;
	int i;

	for (i = 0; i < packages->dim; i++)
	{   Identifier *pid = (Identifier *)packages->data[i];

	    buf.writestring(pid->toChars());
#if _WIN32
	    buf.writeByte('\\');
#else
	    buf.writeByte('/');
#endif
	}
	buf.writestring(filename);
	buf.writeByte(0);
	filename = (char *)buf.extractData();
    }

    m = new Module(filename, ident, 0, 0);
    m->loc = loc;

    /* Search along global.path for .di file, then .d file.
     */
    char *result = NULL;
    FileName *fdi = FileName::forceExt(filename, global.hdr_ext);
    FileName *fd  = FileName::forceExt(filename, global.mars_ext);
    char *sdi = fdi->toChars();
    char *sd  = fd->toChars();

    if (FileName::exists(sdi))
	result = sdi;
    else if (FileName::exists(sd))
	result = sd;
    else if (FileName::absolute(filename))
	;
    else if (!global.path)
	;
    else
    {
	for (size_t i = 0; i < global.path->dim; i++)
	{
	    char *p = (char *)global.path->data[i];
	    char *n = FileName::combine(p, sdi);
	    if (FileName::exists(n))
	    {	result = n;
		break;
	    }
	    mem.free(n);
	    n = FileName::combine(p, sd);
	    if (FileName::exists(n))
	    {	result = n;
		break;
	    }
	    mem.free(n);
	}
    }
    if (result)
	m->srcfile = new File(result);

    if (global.params.verbose)
    {
	printf("import    ");
	if (packages)
	{
	    for (size_t i = 0; i < packages->dim; i++)
	    {   Identifier *pid = (Identifier *)packages->data[i];
		printf("%s.", pid->toChars());
	    }
	}
	printf("%s\t(%s)\n", ident->toChars(), m->srcfile->toChars());
    }

    m->read(loc);
    m->parse();

#ifdef IN_GCC
    d_gcc_magic_module(m);
#endif

    return m;
}

void Module::read(Loc loc)
{
    //printf("Module::read('%s') file '%s'\n", toChars(), srcfile->toChars());
    if (srcfile->read())
    {	error(loc, "cannot read file '%s'", srcfile->toChars());
	fatal();
    }
}

inline unsigned readwordLE(unsigned short *p)
{
#if __I86__
    return *p;
#else
    return (((unsigned char *)p)[1] << 8) | ((unsigned char *)p)[0];
#endif
}

inline unsigned readwordBE(unsigned short *p)
{
    return (((unsigned char *)p)[0] << 8) | ((unsigned char *)p)[1];
}

inline unsigned readlongLE(unsigned *p)
{
#if __I86__
    return *p;
#else
    return ((unsigned char *)p)[0] |
	(((unsigned char *)p)[1] << 8) |
	(((unsigned char *)p)[2] << 16) |
	(((unsigned char *)p)[3] << 24);
#endif
}

inline unsigned readlongBE(unsigned *p)
{
    return ((unsigned char *)p)[3] |
	(((unsigned char *)p)[2] << 8) |
	(((unsigned char *)p)[1] << 16) |
	(((unsigned char *)p)[0] << 24);
}

#if IN_GCC
void Module::parse(bool dump_source)
#else
void Module::parse()
#endif
{   char *srcname;
    unsigned char *buf;
    unsigned buflen;
    unsigned le;
    unsigned bom;

    //printf("Module::parse()\n");

    srcname = srcfile->name->toChars();
    //printf("Module::parse(srcname = '%s')\n", srcname);

    buf = srcfile->buffer;
    buflen = srcfile->len;

    if (buflen >= 2)
    {
	/* Convert all non-UTF-8 formats to UTF-8.
	 * BOM : http://www.unicode.org/faq/utf_bom.html
	 * 00 00 FE FF	UTF-32BE, big-endian
	 * FF FE 00 00	UTF-32LE, little-endian
	 * FE FF	UTF-16BE, big-endian
	 * FF FE	UTF-16LE, little-endian
	 * EF BB BF	UTF-8
	 */

	bom = 1;		// assume there's a BOM
	if (buf[0] == 0xFF && buf[1] == 0xFE)
	{
	    if (buflen >= 4 && buf[2] == 0 && buf[3] == 0)
	    {	// UTF-32LE
		le = 1;

	    Lutf32:
		OutBuffer dbuf;
		unsigned *pu = (unsigned *)(buf);
		unsigned *pumax = &pu[buflen / 4];

		if (buflen & 3)
		{   error("odd length of UTF-32 char source %u", buflen);
		    fatal();
		}

		dbuf.reserve(buflen / 4);
		for (pu += bom; pu < pumax; pu++)
		{   unsigned u;

		    u = le ? readlongLE(pu) : readlongBE(pu);
		    if (u & ~0x7F)
		    {
			if (u > 0x10FFFF)
			{   error("UTF-32 value %08x greater than 0x10FFFF", u);
			    fatal();
			}
			dbuf.writeUTF8(u);
		    }
		    else
			dbuf.writeByte(u);
		}
		dbuf.writeByte(0);		// add 0 as sentinel for scanner
		buflen = dbuf.offset - 1;	// don't include sentinel in count
		buf = (unsigned char *) dbuf.extractData();
	    }
	    else
	    {   // UTF-16LE (X86)
		// Convert it to UTF-8
		le = 1;

	    Lutf16:
		OutBuffer dbuf;
		unsigned short *pu = (unsigned short *)(buf);
		unsigned short *pumax = &pu[buflen / 2];

		if (buflen & 1)
		{   error("odd length of UTF-16 char source %u", buflen);
		    fatal();
		}

		dbuf.reserve(buflen / 2);
		for (pu += bom; pu < pumax; pu++)
		{   unsigned u;

		    u = le ? readwordLE(pu) : readwordBE(pu);
		    if (u & ~0x7F)
		    {	if (u >= 0xD800 && u <= 0xDBFF)
			{   unsigned u2;

			    if (++pu > pumax)
			    {   error("surrogate UTF-16 high value %04x at EOF", u);
				fatal();
			    }
			    u2 = le ? readwordLE(pu) : readwordBE(pu);
			    if (u2 < 0xDC00 || u2 > 0xDFFF)
			    {   error("surrogate UTF-16 low value %04x out of range", u2);
				fatal();
			    }
			    u = (u - 0xD7C0) << 10;
			    u |= (u2 - 0xDC00);
			}
			else if (u >= 0xDC00 && u <= 0xDFFF)
			{   error("unpaired surrogate UTF-16 value %04x", u);
			    fatal();
			}
			else if (u == 0xFFFE || u == 0xFFFF)
			{   error("illegal UTF-16 value %04x", u);
			    fatal();
			}
			dbuf.writeUTF8(u);
		    }
		    else
			dbuf.writeByte(u);
		}
		dbuf.writeByte(0);		// add 0 as sentinel for scanner
		buflen = dbuf.offset - 1;	// don't include sentinel in count
		buf = (unsigned char *) dbuf.extractData();
	    }
	}
	else if (buf[0] == 0xFE && buf[1] == 0xFF)
	{   // UTF-16BE
	    le = 0;
	    goto Lutf16;
	}
	else if (buflen >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 0xFE && buf[3] == 0xFF)
	{   // UTF-32BE
	    le = 0;
	    goto Lutf32;
	}
	else if (buflen >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
	{   // UTF-8

	    buf += 3;
	    buflen -= 3;
	}
	else
	{
	    /* There is no BOM. Make use of Arcane Jill's insight that
	     * the first char of D source must be ASCII to
	     * figure out the encoding.
	     */

	    bom = 0;
	    if (buflen >= 4)
	    {   if (buf[1] == 0 && buf[2] == 0 && buf[3] == 0)
		{   // UTF-32LE
		    le = 1;
		    goto Lutf32;
		}
		else if (buf[0] == 0 && buf[1] == 0 && buf[2] == 0)
		{   // UTF-32BE
		    le = 0;
		    goto Lutf32;
		}
	    }
	    if (buflen >= 2)
	    {
		if (buf[1] == 0)
		{   // UTF-16LE
		    le = 1;
		    goto Lutf16;
		}
		else if (buf[0] == 0)
		{   // UTF-16BE
		    le = 0;
		    goto Lutf16;
		}
	    }

	    // It's UTF-8
	    if (buf[0] >= 0x80)
	    {	error("source file must start with BOM or ASCII character, not \\x%02X", buf[0]);
		fatal();
	    }
	}
    }

#ifdef IN_GCC
    // dump utf-8 encoded source
    if (dump_source)
    {	// %% srcname could contain a path ...
	d_gcc_dump_source(srcname, "utf-8", buf, buflen);
    }
#endif

    /* If it starts with the string "Ddoc", then it's a documentation
     * source file.
     */
    if (buflen >= 4 && memcmp(buf, "Ddoc", 4) == 0)
    {
	comment = buf + 4;
	isDocFile = 1;
	return;
    }
    if (isHtml)
    {
	OutBuffer *dbuf = new OutBuffer();
	Html h(srcname, buf, buflen);
	h.extractCode(dbuf);
	buf = dbuf->data;
	buflen = dbuf->offset;
#ifdef IN_GCC
	// dump extracted source
	if (dump_source)
	    d_gcc_dump_source(srcname, "d.utf-8", buf, buflen);
#endif
    }
    Parser p(this, buf, buflen, docfile != NULL);
    p.nextToken();
    members = p.parseModule();
    md = p.md;
    numlines = p.loc.linnum;

    DsymbolTable *dst;

    if (md)
    {	this->ident = md->id;
	dst = Package::resolve(md->packages, &this->parent, NULL);
    }
    else
    {
	dst = modules;

	/* Check to see if module name is a valid identifier
	 */
	if (!Lexer::isValidIdentifier(this->ident->toChars()))
	    error("has non-identifier characters in filename, use module declaration instead");
    }

    // Update global list of modules
    if (!dst->insert(this))
    {
	if (md)
	    error(loc, "is in multiple packages %s", md->toChars());
	else
	    error(loc, "is in multiple defined");
    }
    else
    {
	amodules.push(this);
    }
}

void Module::semantic(Scope* unused_sc)
{   int i;

    if (semanticstarted)
	return;

    //printf("+Module::semantic(this = %p, '%s'): parent = %p\n", this, toChars(), parent);
    semanticstarted = 1;

    // Note that modules get their own scope, from scratch.
    // This is so regardless of where in the syntax a module
    // gets imported, it is unaffected by context.
    Scope *sc = Scope::createGlobal(this);	// create root scope

    //printf("Module = %p, linkage = %d\n", sc->scopesym, sc->linkage);

    // Add import of "object" if this module isn't "object"
    if (ident != Id::object)
    {
	Import *im = new Import(0, NULL, Id::object, NULL, 0);
	members->shift(im);
    }

    // Add all symbols into module's symbol table
    symtab = new DsymbolTable();
    for (i = 0; i < members->dim; i++)
    {	Dsymbol *s;

	s = (Dsymbol *)members->data[i];
	s->addMember(NULL, sc->scopesym, 1);
    }

    // Pass 1 semantic routines: do public side of the definition
    for (i = 0; i < members->dim; i++)
    {	Dsymbol *s;

	s = (Dsymbol *)members->data[i];
	//printf("\tModule('%s'): '%s'.semantic()\n", toChars(), s->toChars());
	s->semantic(sc);
	runDeferredSemantic();
    }

    sc = sc->pop();
    sc->pop();
    semanticdone = semanticstarted;
    //printf("-Module::semantic(this = %p, '%s'): parent = %p\n", this, toChars(), parent);
}

void Module::semantic2(Scope* unused_sc)
{   int i;

    if (deferred.dim)
    {
	for (int i = 0; i < deferred.dim; i++)
	{
	    Dsymbol *sd = (Dsymbol *)deferred.data[i];

	    sd->error("unable to resolve forward reference in definition");
	}
	return;
    }
    //printf("Module::semantic2('%s'): parent = %p\n", toChars(), parent);
    if (semanticstarted >= 2)
	return;
    assert(semanticstarted == 1);
    semanticstarted = 2;

    // Note that modules get their own scope, from scratch.
    // This is so regardless of where in the syntax a module
    // gets imported, it is unaffected by context.
    Scope *sc = Scope::createGlobal(this);	// create root scope
    //printf("Module = %p\n", sc.scopesym);

    // Pass 2 semantic routines: do initializers and function bodies
    for (i = 0; i < members->dim; i++)
    {	Dsymbol *s;

	s = (Dsymbol *)members->data[i];
	s->semantic2(sc);
    }

    sc = sc->pop();
    sc->pop();
    semanticdone = semanticstarted;
    //printf("-Module::semantic2('%s'): parent = %p\n", toChars(), parent);
}

void Module::semantic3(Scope* unused_sc)
{   int i;

    //printf("Module::semantic3('%s'): parent = %p\n", toChars(), parent);
    if (semanticstarted >= 3)
	return;
    assert(semanticstarted == 2);
    semanticstarted = 3;

    // Note that modules get their own scope, from scratch.
    // This is so regardless of where in the syntax a module
    // gets imported, it is unaffected by context.
    Scope *sc = Scope::createGlobal(this);	// create root scope
    //printf("Module = %p\n", sc.scopesym);

    // Pass 3 semantic routines: do initializers and function bodies
    for (i = 0; i < members->dim; i++)
    {	Dsymbol *s;

	s = (Dsymbol *)members->data[i];
	//printf("Module %s: %s.semantic3()\n", toChars(), s->toChars());
	s->semantic3(sc);
    }

    sc = sc->pop();
    sc->pop();
    semanticdone = semanticstarted;
}

void Module::inlineScan()
{   int i;

    if (semanticstarted >= 4)
	return;
    assert(semanticstarted == 3);
    semanticstarted = 4;

    // Note that modules get their own scope, from scratch.
    // This is so regardless of where in the syntax a module
    // gets imported, it is unaffected by context.
    //printf("Module = %p\n", sc.scopesym);

    for (i = 0; i < members->dim; i++)
    {	Dsymbol *s;

	s = (Dsymbol *)members->data[i];
	//if (global.params.verbose)
	    //printf("inline scan symbol %s\n", s->toChars());

	s->inlineScan();
    }
    semanticdone = semanticstarted;
}

/****************************************************
 */

// is this used anywhere?
/*
void Module::gensymfile()
{
    OutBuffer buf;
    HdrGenState hgs;

    //printf("Module::gensymfile()\n");

    buf.printf("// Sym file generated from '%s'", srcfile->toChars());
    buf.writenl();

    for (int i = 0; i < members->dim; i++)
    {	Dsymbol *s = (Dsymbol *)members->data[i];

	s->toCBuffer(&buf, &hgs);
    }

    // Transfer image to file
    symfile->setbuffer(buf.data, buf.offset);
    buf.data = NULL;

    symfile->writev();
}*/

/**********************************
 * Determine if we need to generate an instance of ModuleInfo
 * for this Module.
 */

int Module::needModuleInfo()
{
    return needmoduleinfo;
}

Dsymbol *Module::search(Loc loc, Identifier *ident, int flags)
{
    /* Since modules can be circularly referenced,
     * need to stop infinite recursive searches.
     */

    //printf("%s Module::search('%s', flags = %d) insearch = %d\n", toChars(), ident->toChars(), flags, insearch);
    Dsymbol *s;
    if (insearch)
	s = NULL;
    else if (searchCacheIdent == ident && searchCacheFlags == flags)
	s = searchCacheSymbol;
    else
    {
	insearch = 1;
	s = ScopeDsymbol::search(loc, ident, flags);
	insearch = 0;

	searchCacheIdent = ident;
	searchCacheSymbol = s;
	searchCacheFlags = flags;
    }
    return s;
}

/*******************************************
 * Can't run semantic on s now, try again later.
 */

void Module::addDeferredSemantic(Dsymbol *s)
{
    // Don't add it if it is already there
    for (int i = 0; i < deferred.dim; i++)
    {
	Dsymbol *sd = (Dsymbol *)deferred.data[i];

	if (sd == s)
	    return;
    }

    //printf("Module::addDeferredSemantic('%s')\n", s->toChars());
    deferred.push(s);
}


/******************************************
 * Run semantic() on deferred symbols.
 */

void Module::runDeferredSemantic()
{
    size_t len;

    static int nested;
    if (nested)
	return;
    //if (deferred.dim) printf("+Module::runDeferredSemantic('%s'), len = %d\n", toChars(), deferred.dim);
    nested++;

    do
    {
	dprogress = 0;
	len = deferred.dim;
	if (!len)
	    break;

	Dsymbol **todo;
	Dsymbol *tmp;
	if (len == 1)
	{
	    todo = &tmp;
	}
	else
	{
	    todo = (Dsymbol **)alloca(len * sizeof(Dsymbol *));
	    assert(todo);
	}
	memcpy(todo, deferred.data, len * sizeof(Dsymbol *));
	deferred.setDim(0);

	for (int i = 0; i < len; i++)
	{
	    Dsymbol *s = todo[i];

	    s->semantic(NULL);
	    //printf("deferred: %s, parent = %s\n", s->toChars(), s->parent->toChars());
	}
	//printf("\tdeferred.dim = %d, len = %d, dprogress = %d\n", deferred.dim, len, dprogress);
    } while (deferred.dim < len || dprogress);	// while making progress
    nested--;
    //printf("-Module::runDeferredSemantic('%s'), len = %d\n", toChars(), deferred.dim);
}

/************************************
 * Recursively look at every module this module imports,
 * return TRUE if it imports m.
 * Can be used to detect circular imports.
 */

int Module::imports(Module *m)
{
    //printf("%s Module::imports(%s)\n", toChars(), m->toChars());
    int aimports_dim = aimports.dim;
#if 0
    for (int i = 0; i < aimports.dim; i++)
    {	Module *mi = (Module *)aimports.data[i];
	printf("\t[%d] %s\n", i, mi->toChars());
    }
#endif
    for (int i = 0; i < aimports.dim; i++)
    {	Module *mi = (Module *)aimports.data[i];
	if (mi == m)
	    return TRUE;
	if (!mi->insearch)
	{
	    mi->insearch = 1;
	    int r = mi->imports(m);
	    mi->insearch = 0;
	    if (r)
		return r;
	}
    }
    return FALSE;
}

/* =========================== ModuleDeclaration ===================== */

ModuleDeclaration::ModuleDeclaration(Array *packages, Identifier *id)
{
    this->packages = packages;
    this->id = id;
}

char *ModuleDeclaration::toChars()
{
    OutBuffer buf;
    int i;

    if (packages && packages->dim)
    {
	for (i = 0; i < packages->dim; i++)
	{   Identifier *pid = (Identifier *)packages->data[i];

	    buf.writestring(pid->toChars());
	    buf.writeByte('.');
	}
    }
    buf.writestring(id->toChars());
    buf.writeByte(0);
    return (char *)buf.extractData();
}

/* =========================== Package ===================== */

Package::Package(Identifier *ident)
	: ScopeDsymbol(ident)
{
}


const char *Package::kind()
{
    return "package";
}


DsymbolTable *Package::resolve(Array *packages, Dsymbol **pparent, Package **ppkg)
{
    DsymbolTable *dst = Module::modules;
    Dsymbol *parent = NULL;

    //printf("Package::resolve()\n");
    if (ppkg)
	*ppkg = NULL;

    if (packages)
    {   int i;

	for (i = 0; i < packages->dim; i++)
	{   Identifier *pid = (Identifier *)packages->data[i];
	    Dsymbol *p;

	    p = dst->lookup(pid);
	    if (!p)
	    {
		p = new Package(pid);
		dst->insert(p);
		p->parent = parent;
		((ScopeDsymbol *)p)->symtab = new DsymbolTable();
	    }
	    else
	    {
		assert(p->isPackage());
		if (p->isModule())
		{   p->error("module and package have the same name");
		    fatal();
		    break;
		}
	    }
	    parent = p;
	    dst = ((Package *)p)->symtab;
	    if (ppkg && !*ppkg)
		*ppkg = (Package *)p;
	}
	if (pparent)
	{
	    *pparent = parent;
	}
    }
    return dst;
}

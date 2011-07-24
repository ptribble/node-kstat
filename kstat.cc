#include <v8.h>
#include <node.h>
#include <string.h>
#include <unistd.h>
#include <node_object_wrap.h>
#include <kstat.h>
#include <errno.h>
#include <string>
#include <vector>
#include <sys/varargs.h>

#include <nfs/nfs.h>
#include <nfs/nfs_clnt.h>
#include <sys/sysinfo.h>
#include <sys/var.h>
#include <sys/dnlc.h>

#define TOSTR(obj) (*String::Utf8Value(obj))

using namespace v8;
using std::string;
using std::vector;

class KStatReader : node::ObjectWrap {
public:
	static void Initialize(Handle<Object> target);

protected:
	static Persistent<FunctionTemplate> KStatReader::templ;

	KStatReader(string *module, string *classname,
	    string *name, int instance);
	Handle<Value> error(const char *fmt, ...);
	Handle<Value> read(kstat_t *);
	Handle<Value> list(kstat_t *);
	int getkcid();
	int update();
	~KStatReader();

	static Handle<Value> New(const Arguments& args);
	static Handle<Value> Read(const Arguments& args);
	static Handle<Value> List(const Arguments& args);
	static Handle<Value> Update(const Arguments& args);
	static Handle<Value> getKCID(const Arguments& args);
	static Handle<Value> getKstat(const Arguments& args);


private:
	static string *stringMember(Local<Value>, char *, char *);
	static int64_t intMember(Local<Value>, char *, int64_t);
	Handle<Object> data_named(kstat_t *);
	Handle<Object> data_intr(kstat_t *);
	Handle<Object> data_io(kstat_t *);
	Handle<Object> data_raw(kstat_t *);

	string *ksr_module;
	string *ksr_class;
	string *ksr_name;
	int ksr_instance;
	kid_t ksr_kid;
	kstat_ctl_t *ksr_ctl;
	vector<kstat_t *> ksr_kstats;
};

Persistent<FunctionTemplate> KStatReader::templ;

KStatReader::KStatReader(string *module, string *classname,
    string *name, int instance)
    : node::ObjectWrap(), ksr_module(module), ksr_class(classname),
    ksr_name(name), ksr_instance(instance), ksr_kid(-1)
{
	if ((ksr_ctl = kstat_open()) == NULL)
		throw "could not open kstat";
};

KStatReader::~KStatReader()
{
	delete ksr_module;
	delete ksr_class;
	delete ksr_name;
	kstat_close(ksr_ctl);
}

int
KStatReader::getkcid()
{
	return((int) ksr_ctl->kc_chain_id);
}

int
KStatReader::update()
{
	kstat_t *ksp;
	kid_t kid;

	if ((kid = kstat_chain_update(ksr_ctl)) == 0 && ksr_kid != -1)
		return (0);

	if (kid == -1)
		return (-1);

	ksr_kid = kid;
	ksr_kstats.clear();

	for (ksp = ksr_ctl->kc_chain; ksp != NULL; ksp = ksp->ks_next) {
		if (!ksr_module->empty() &&
		    ksr_module->compare(ksp->ks_module) != 0)
			continue;

		if (!ksr_class->empty() &&
		    ksr_class->compare(ksp->ks_class) != 0)
			continue;

		if (!ksr_name->empty() && ksr_name->compare(ksp->ks_name) != 0)
			continue;

		if (ksr_instance != -1 && ksp->ks_instance != ksr_instance)
			continue;

		ksr_kstats.push_back(ksp);
	}

	return (0);
}

void
KStatReader::Initialize(Handle<Object> target)
{
	HandleScope scope;

	Local<FunctionTemplate> k = FunctionTemplate::New(KStatReader::New);

	templ = Persistent<FunctionTemplate>::New(k);
	templ->InstanceTemplate()->SetInternalFieldCount(1);
	templ->SetClassName(String::NewSymbol("Reader"));

	NODE_SET_PROTOTYPE_METHOD(templ, "read", KStatReader::Read);

	NODE_SET_PROTOTYPE_METHOD(templ, "list", KStatReader::List);

	NODE_SET_PROTOTYPE_METHOD(templ, "getkcid", KStatReader::getKCID);

	NODE_SET_PROTOTYPE_METHOD(templ, "getkstat", KStatReader::getKstat);

	NODE_SET_PROTOTYPE_METHOD(templ, "chainupdate", KStatReader::Update);

	target->Set(String::NewSymbol("Reader"), templ->GetFunction());
}

string *
KStatReader::stringMember(Local<Value> value, char *member, char *deflt)
{
	if (!value->IsObject())
		return (new string (deflt));

	Local<Object> o = Local<Object>::Cast(value);
	Local<Value> v = o->Get(String::New(member));

	if (!v->IsString())
		return (new string (deflt));

	String::AsciiValue val(v);
	return (new string(*val));
}

int64_t
KStatReader::intMember(Local<Value> value, char *member, int64_t deflt)
{
	int64_t rval = deflt;

	if (!value->IsObject())
		return (rval);

	Local<Object> o = Local<Object>::Cast(value);
	value = o->Get(String::New(member));

	if (!value->IsNumber())
		return (rval);

	Local<Integer> i = Local<Integer>::Cast(value);

	return (i->Value());
}

Handle<Value>
KStatReader::New(const Arguments& args)
{
	HandleScope scope;

	KStatReader *k = new KStatReader(stringMember(args[0], "module", ""),
	    stringMember(args[0], "class", ""),
	    stringMember(args[0], "name", ""),
	    intMember(args[0], "instance", -1));

	k->Wrap(args.Holder());

	return (args.This());
}

Handle<Value>
KStatReader::error(const char *fmt, ...)
{
	char buf[1024], buf2[1024];
	char *err = buf;
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);

	if (buf[strlen(buf) - 1] != '\n') {
		/*
		 * If our error doesn't end in a new-line, we'll append the
		 * strerror of errno.
		 */
		(void) snprintf(err = buf2, sizeof (buf2),
		    "%s: %s", buf, strerror(errno));
	} else {
		buf[strlen(buf) - 1] = '\0';
	}

	return (ThrowException(Exception::Error(String::New(err))));
}

Handle<Object>
KStatReader::data_named(kstat_t *ksp)
{
	Handle<Object> data = Object::New();
	kstat_named_t *nm = KSTAT_NAMED_PTR(ksp);
	int i;

	assert(ksp->ks_type == KSTAT_TYPE_NAMED);

	for (i = 0; i < ksp->ks_ndata; i++, nm++) {
		Handle<Value> val;

		switch (nm->data_type) {
		case KSTAT_DATA_CHAR:
			val = Number::New(nm->value.c[0]);
			break;

		case KSTAT_DATA_INT32:
			val = Number::New(nm->value.i32);
			break;

		case KSTAT_DATA_UINT32:
			val = Number::New(nm->value.ui32);
			break;

		case KSTAT_DATA_INT64:
			val = Number::New(nm->value.i64);
			break;

		case KSTAT_DATA_UINT64:
			val = Number::New(nm->value.ui64);
			break;

		case KSTAT_DATA_STRING:
			val = String::New(KSTAT_NAMED_STR_PTR(nm));
			break;

		default:
			throw (error("unrecognized data type %d for member "
			    "\"%s\" in instance %d of stat \"%s\" (module "
			    "\"%s\", class \"%s\")\n", nm->data_type,
			    nm->name, ksp->ks_instance, ksp->ks_name,
			    ksp->ks_module, ksp->ks_class));
		}

		data->Set(String::New(nm->name), val);
	}

	return (data);
}

Handle<Object>
KStatReader::data_raw(kstat_t *ksp)
{
	Handle<Object> data = Object::New();

	assert(ksp->ks_type == KSTAT_TYPE_RAW);

	if (!strcmp(ksp->ks_module,"unix")) {
	    if (!strcmp(ksp->ks_name, "ncstats")) {
		struct ncstats *ncstatsp = (struct ncstats *)(ksp->ks_data);
		data->Set(String::New("hits"),
			Number::New(ncstatsp->hits));
		data->Set(String::New("misses"),
			Number::New(ncstatsp->misses));
		data->Set(String::New("enters"),
			Number::New(ncstatsp->enters));
		data->Set(String::New("dbl_enters"),
			Number::New(ncstatsp->dbl_enters));
		data->Set(String::New("long_enter"),
			Number::New(ncstatsp->long_enter));
		data->Set(String::New("long_look"),
			Number::New(ncstatsp->long_look));
		data->Set(String::New("move_to_front"),
			Number::New(ncstatsp->move_to_front));
		data->Set(String::New("purges"),
			Number::New(ncstatsp->purges));
	    }
	    if (!strcmp(ksp->ks_name, "var")) {
		struct var *varp = (struct var *)(ksp->ks_data);
		data->Set(String::New("v_buf"),
			Number::New(varp->v_buf));
		data->Set(String::New("v_call"),
			Number::New(varp->v_call));
		data->Set(String::New("v_proc"),
			Number::New(varp->v_proc));
		data->Set(String::New("v_maxupttl"),
			Number::New(varp->v_maxupttl));
		data->Set(String::New("v_nglobpris"),
			Number::New(varp->v_nglobpris));
		data->Set(String::New("v_maxsyspri"),
			Number::New(varp->v_maxsyspri));
		data->Set(String::New("v_clist"),
			Number::New(varp->v_clist));
		data->Set(String::New("v_maxup"),
			Number::New(varp->v_maxup));
		data->Set(String::New("v_hbuf"),
			Number::New(varp->v_hbuf));
		data->Set(String::New("v_hmask"),
			Number::New(varp->v_hmask));
		data->Set(String::New("v_pbuf"),
			Number::New(varp->v_pbuf));
		data->Set(String::New("v_sptmap"),
			Number::New(varp->v_sptmap));
		data->Set(String::New("v_maxpmem"),
			Number::New(varp->v_maxpmem));
		data->Set(String::New("v_autoup"),
			Number::New(varp->v_autoup));
		data->Set(String::New("v_bufhwm"),
			Number::New(varp->v_bufhwm));
	    }
	    if (!strcmp(ksp->ks_name, "sysinfo")) {
		sysinfo_t *sysinfop = (sysinfo_t *)(ksp->ks_data);
		data->Set(String::New("updates"),
			Number::New(sysinfop->updates));
		data->Set(String::New("runque"),
			Number::New(sysinfop->runque));
		data->Set(String::New("runocc"),
			Number::New(sysinfop->runocc));
		data->Set(String::New("swpque"),
			Number::New(sysinfop->swpque));
		data->Set(String::New("swpocc"),
			Number::New(sysinfop->swpocc));
		data->Set(String::New("waiting"),
			Number::New(sysinfop->waiting));
	    }
	    if (!strcmp(ksp->ks_name, "vminfo")) {
		vminfo_t *vminfop = (vminfo_t *)(ksp->ks_data);
		data->Set(String::New("freemem"),
			Number::New(vminfop->freemem));
		data->Set(String::New("swap_resv"),
			Number::New(vminfop->swap_resv));
		data->Set(String::New("swap_alloc"),
			Number::New(vminfop->swap_alloc));
		data->Set(String::New("swap_avail"),
			Number::New(vminfop->swap_avail));
		data->Set(String::New("swap_free"),
			Number::New(vminfop->swap_free));
	    }
	}
	if (!strcmp(ksp->ks_module,"nfs") && !strcmp(ksp->ks_name,"mntinfo")) {
	    struct mntinfo_kstat *mntinfop =
			(struct mntinfo_kstat *)(ksp->ks_data);
	    data->Set(String::New("mik_proto"),
			String::New(mntinfop->mik_proto));
	    data->Set(String::New("mik_vers"),
			Number::New(mntinfop->mik_vers));
	    data->Set(String::New("mik_flags"),
			Number::New(mntinfop->mik_flags));
	    data->Set(String::New("mik_secmod"),
			Number::New(mntinfop->mik_secmod));
	    data->Set(String::New("mik_curread"),
			Number::New(mntinfop->mik_curread));
	    data->Set(String::New("mik_curwrite"),
			Number::New(mntinfop->mik_curwrite));
	    data->Set(String::New("mik_timeo"),
			Number::New(mntinfop->mik_timeo));
	    data->Set(String::New("mik_retrans"),
			Number::New(mntinfop->mik_retrans));
	    data->Set(String::New("mik_acregmin"),
			Number::New(mntinfop->mik_acregmin));
	    data->Set(String::New("mik_acregmax"),
			Number::New(mntinfop->mik_acregmax));
	    data->Set(String::New("mik_acdirmin"),
			Number::New(mntinfop->mik_acdirmin));
	    data->Set(String::New("mik_acdirmax"),
			Number::New(mntinfop->mik_acdirmax));
	    data->Set(String::New("mik_noresponse"),
			Number::New(mntinfop->mik_noresponse));
	    data->Set(String::New("mik_failover"),
			Number::New(mntinfop->mik_failover));
	    data->Set(String::New("mik_remap"),
			Number::New(mntinfop->mik_remap));
	    data->Set(String::New("mik_curserver"),
			String::New(mntinfop->mik_curserver));
	    data->Set(String::New("lookup_srtt"),
			Number::New(mntinfop->mik_timers[0].srtt));
	    data->Set(String::New("lookup_deviate"),
			Number::New(mntinfop->mik_timers[0].deviate));
	    data->Set(String::New("lookup_rtxcur"),
			Number::New(mntinfop->mik_timers[0].rtxcur));
	    data->Set(String::New("read_srtt"),
			Number::New(mntinfop->mik_timers[1].srtt));
	    data->Set(String::New("read_deviate"),
			Number::New(mntinfop->mik_timers[1].deviate));
	    data->Set(String::New("read_rtxcur"),
			Number::New(mntinfop->mik_timers[1].rtxcur));
	    data->Set(String::New("write_srtt"),
			Number::New(mntinfop->mik_timers[2].srtt));
	    data->Set(String::New("write_deviate"),
			Number::New(mntinfop->mik_timers[2].deviate));
	    data->Set(String::New("write_rtxcur"),
			Number::New(mntinfop->mik_timers[2].rtxcur));
	}

	if (!strcmp(ksp->ks_module,"cpu_stat")) {
	    cpu_stat_t *statp = (cpu_stat_t *)(ksp->ks_data);
	    cpu_sysinfo_t *sysinfop = &statp->cpu_sysinfo;
	    cpu_syswait_t *syswaitp = &statp->cpu_syswait;
	    cpu_vminfo_t *vminfop = &statp->cpu_vminfo;

	    data->Set(String::New("idle"),
			Number::New(sysinfop->cpu[CPU_IDLE]));
	    data->Set(String::New("user"),
			Number::New(sysinfop->cpu[CPU_USER]));
	    data->Set(String::New("kernel"),
			Number::New(sysinfop->cpu[CPU_KERNEL]));
	    data->Set(String::New("wait"),
			Number::New(sysinfop->cpu[CPU_WAIT]));
	    data->Set(String::New("wait_io"),
			Number::New(sysinfop->wait[W_IO]));
	    data->Set(String::New("wait_swap"),
			Number::New(sysinfop->wait[W_SWAP]));
	    data->Set(String::New("wait_pio"),
			Number::New(sysinfop->wait[W_PIO]));
	    data->Set(String::New("bread"),
			Number::New(sysinfop->bread));
	    data->Set(String::New("bwrite"),
			Number::New(sysinfop->bwrite));
	    data->Set(String::New("lread"),
			Number::New(sysinfop->lread));
	    data->Set(String::New("lwrite"),
			Number::New(sysinfop->lwrite));
	    data->Set(String::New("phread"),
			Number::New(sysinfop->phread));
	    data->Set(String::New("phwrite"),
			Number::New(sysinfop->phwrite));
	    data->Set(String::New("pswitch"),
			Number::New(sysinfop->pswitch));
	    data->Set(String::New("trap"),
			Number::New(sysinfop->trap));
	    data->Set(String::New("intr"),
			Number::New(sysinfop->intr));
	    data->Set(String::New("syscall"),
			Number::New(sysinfop->syscall));
	    data->Set(String::New("sysread"),
			Number::New(sysinfop->sysread));
	    data->Set(String::New("syswrite"),
			Number::New(sysinfop->syswrite));
	    data->Set(String::New("sysfork"),
			Number::New(sysinfop->sysfork));
	    data->Set(String::New("sysvfork"),
			Number::New(sysinfop->sysvfork));
	    data->Set(String::New("sysexec"),
			Number::New(sysinfop->sysexec));
	    data->Set(String::New("readch"),
			Number::New(sysinfop->readch));
	    data->Set(String::New("writech"),
			Number::New(sysinfop->writech));
	    data->Set(String::New("rawch"),
			Number::New(sysinfop->rawch));
	    data->Set(String::New("canch"),
			Number::New(sysinfop->canch));
	    data->Set(String::New("outch"),
			Number::New(sysinfop->outch));
	    data->Set(String::New("msg"),
			Number::New(sysinfop->msg));
	    data->Set(String::New("sema"),
			Number::New(sysinfop->sema));
	    data->Set(String::New("namei"),
			Number::New(sysinfop->namei));
	    data->Set(String::New("ufsiget"),
			Number::New(sysinfop->ufsiget));
	    data->Set(String::New("ufsdirblk"),
			Number::New(sysinfop->ufsdirblk));
	    data->Set(String::New("ufsipage"),
			Number::New(sysinfop->ufsipage));
	    data->Set(String::New("ufsinopage"),
			Number::New(sysinfop->ufsinopage));
	    data->Set(String::New("inodeovf"),
			Number::New(sysinfop->inodeovf));
	    data->Set(String::New("fileovf"),
			Number::New(sysinfop->fileovf));
	    data->Set(String::New("procovf"),
			Number::New(sysinfop->procovf));
	    data->Set(String::New("intrthread"),
			Number::New(sysinfop->intrthread));
	    data->Set(String::New("intrblk"),
			Number::New(sysinfop->intrblk));
	    data->Set(String::New("idlethread"),
			Number::New(sysinfop->idlethread));
	    data->Set(String::New("inv_swtch"),
			Number::New(sysinfop->inv_swtch));
	    data->Set(String::New("nthreads"),
			Number::New(sysinfop->nthreads));
	    data->Set(String::New("cpumigrate"),
			Number::New(sysinfop->cpumigrate));
	    data->Set(String::New("xcalls"),
			Number::New(sysinfop->xcalls));
	    data->Set(String::New("mutex_adenters"),
			Number::New(sysinfop->mutex_adenters));
	    data->Set(String::New("rw_rdfails"),
			Number::New(sysinfop->rw_rdfails));
	    data->Set(String::New("rw_wrfails"),
			Number::New(sysinfop->rw_wrfails));
	    data->Set(String::New("modload"),
			Number::New(sysinfop->modload));
	    data->Set(String::New("modunload"),
			Number::New(sysinfop->modunload));
	    data->Set(String::New("bawrite"),
			Number::New(sysinfop->bawrite));
	    data->Set(String::New("iowait"),
			Number::New(syswaitp->iowait));
	    data->Set(String::New("pgrec"),
			Number::New(vminfop->pgrec));
	    data->Set(String::New("pgfrec"),
			Number::New(vminfop->pgfrec));
	    data->Set(String::New("pgin"),
			Number::New(vminfop->pgin));
	    data->Set(String::New("pgpgin"),
			Number::New(vminfop->pgpgin));
	    data->Set(String::New("pgout"),
			Number::New(vminfop->pgout));
	    data->Set(String::New("pgpgout"),
			Number::New(vminfop->pgpgout));
	    data->Set(String::New("swapin"),
			Number::New(vminfop->swapin));
	    data->Set(String::New("pgswapin"),
			Number::New(vminfop->pgswapin));
	    data->Set(String::New("swapout"),
			Number::New(vminfop->swapout));
	    data->Set(String::New("pgswapout"),
			Number::New(vminfop->pgswapout));
	    data->Set(String::New("zfod"),
			Number::New(vminfop->zfod));
	    data->Set(String::New("dfree"),
			Number::New(vminfop->dfree));
	    data->Set(String::New("scan"),
			Number::New(vminfop->scan));
	    data->Set(String::New("rev"),
			Number::New(vminfop->rev));
	    data->Set(String::New("hat_fault"),
			Number::New(vminfop->hat_fault));
	    data->Set(String::New("as_fault"),
			Number::New(vminfop->as_fault));
	    data->Set(String::New("maj_fault"),
			Number::New(vminfop->maj_fault));
	    data->Set(String::New("cow_fault"),
			Number::New(vminfop->cow_fault));
	    data->Set(String::New("prot_fault"),
			Number::New(vminfop->prot_fault));
	    data->Set(String::New("softlock"),
			Number::New(vminfop->softlock));
	    data->Set(String::New("kernel_asflt"),
			Number::New(vminfop->kernel_asflt));
	    data->Set(String::New("pgrrun"),
			Number::New(vminfop->pgrrun));
	    data->Set(String::New("execpgin"),
			Number::New(vminfop->execpgin));
	    data->Set(String::New("execpgout"),
			Number::New(vminfop->execpgout));
	    data->Set(String::New("execfree"),
			Number::New(vminfop->execfree));
	    data->Set(String::New("anonpgin"),
			Number::New(vminfop->anonpgin));
	    data->Set(String::New("anonpgout"),
			Number::New(vminfop->anonpgout));
	    data->Set(String::New("anonfree"),
			Number::New(vminfop->anonfree));
	    data->Set(String::New("fspgin"),
			Number::New(vminfop->fspgin));
	    data->Set(String::New("fspgout"),
			Number::New(vminfop->fspgout));
	    data->Set(String::New("fsfree"),
			Number::New(vminfop->fsfree));
	}

	return (data);
}

Handle<Object>
KStatReader::data_intr(kstat_t *ksp)
{
	Handle<Object> data = Object::New();
	kstat_intr_t *intr = KSTAT_INTR_PTR(ksp);

	assert(ksp->ks_type == KSTAT_TYPE_INTR);

	data->Set(String::New("hard"),
		Integer::New(intr->intrs[KSTAT_INTR_HARD]));
	data->Set(String::New("soft"),
		Integer::New(intr->intrs[KSTAT_INTR_SOFT]));
	data->Set(String::New("watchdog"),
		Integer::New(intr->intrs[KSTAT_INTR_SOFT]));
	data->Set(String::New("spurious"),
		Integer::New(intr->intrs[KSTAT_INTR_SPURIOUS]));
	data->Set(String::New("multiple_service"),
		Integer::New(intr->intrs[KSTAT_INTR_MULTSVC]));

	return (data);
}

Handle<Object>
KStatReader::data_io(kstat_t *ksp)
{
	Handle<Object> data = Object::New();
	kstat_io_t *io = KSTAT_IO_PTR(ksp);

	assert(ksp->ks_type == KSTAT_TYPE_IO);

	data->Set(String::New("nread"), Number::New(io->nread));
	data->Set(String::New("nwritten"), Number::New(io->nwritten));
	data->Set(String::New("reads"), Integer::New(io->reads));
	data->Set(String::New("writes"), Integer::New(io->writes));

	data->Set(String::New("wtime"), Number::New(io->wtime));
	data->Set(String::New("wlentime"), Number::New(io->wlentime));
	data->Set(String::New("wlastupdate"), Number::New(io->wlastupdate));

	data->Set(String::New("rtime"), Number::New(io->rtime));
	data->Set(String::New("rlentime"), Number::New(io->rlentime));
	data->Set(String::New("rlastupdate"), Number::New(io->rlastupdate));

	data->Set(String::New("wcnt"), Integer::New(io->wcnt));
	data->Set(String::New("rcnt"), Integer::New(io->rcnt));

	return (data);
}

Handle<Value>
KStatReader::read(kstat_t *ksp)
{
	Handle<Object> rval = Object::New();
	Handle<Object> data;

	rval->Set(String::New("class"), String::New(ksp->ks_class));
	rval->Set(String::New("module"), String::New(ksp->ks_module));
	rval->Set(String::New("name"), String::New(ksp->ks_name));
	rval->Set(String::New("instance"), Integer::New(ksp->ks_instance));
	rval->Set(String::New("type"), Integer::New(ksp->ks_type));

	if (kstat_read(ksr_ctl, ksp, NULL) == -1) {
		/*
		 * It is deeply annoying, but some kstats can return errors
		 * under otherwise routine conditions.  (ACPI is one
		 * offender; there are surely others.)  To prevent these
		 * fouled kstats from completely ruining our day, we assign
		 * an "error" member to the return value that consists of
		 * the strerror().
		 */
		rval->Set(String::New("error"), String::New(strerror(errno)));
		return (rval);
	}

	rval->Set(String::New("snaptime"), Number::New(ksp->ks_snaptime));
	rval->Set(String::New("crtime"), Number::New(ksp->ks_crtime));

	if (ksp->ks_type == KSTAT_TYPE_NAMED) {
		data = data_named(ksp);
	} else if (ksp->ks_type == KSTAT_TYPE_IO) {
		data = data_io(ksp);
	} else if (ksp->ks_type == KSTAT_TYPE_INTR) {
		data = data_intr(ksp);
	} else if (ksp->ks_type == KSTAT_TYPE_RAW) {
		data = data_raw(ksp);
	} else {
		return (rval);
	}

	rval->Set(String::New("data"), data);

	return (rval);
}

Handle<Value>
KStatReader::Read(const Arguments& args)
{
	KStatReader *k = ObjectWrap::Unwrap<KStatReader>(args.Holder());
	Handle<Array> rval;
	HandleScope scope;
	int i;

	if (k->update() == -1)
		return (k->error("failed to update kstat chain"));

	rval = Array::New(k->ksr_kstats.size());

	try {
		for (i = 0; i < k->ksr_kstats.size(); i++)
			rval->Set(i, k->read(k->ksr_kstats[i]));
	} catch (Handle<Value> err) {
		return (err);
	}

	return (rval);
}

Handle<Value>
KStatReader::list(kstat_t *ksp)
{
	Handle<Object> rval = Object::New();
	Handle<Object> data;

	rval->Set(String::New("class"), String::New(ksp->ks_class));
	rval->Set(String::New("module"), String::New(ksp->ks_module));
	rval->Set(String::New("name"), String::New(ksp->ks_name));
	rval->Set(String::New("instance"), Integer::New(ksp->ks_instance));
	rval->Set(String::New("type"), Integer::New(ksp->ks_type));
	rval->Set(String::New("snaptime"), Number::New(ksp->ks_snaptime));
	rval->Set(String::New("crtime"), Number::New(ksp->ks_crtime));

	return (rval);
}

Handle<Value>
KStatReader::List(const Arguments& args)
{
	KStatReader *k = ObjectWrap::Unwrap<KStatReader>(args.Holder());
	Handle<Array> rval;
	HandleScope scope;
	int i;

	if (k->update() == -1)
		return (k->error("failed to update kstat chain"));

	rval = Array::New(k->ksr_kstats.size());

	try {
		for (i = 0; i < k->ksr_kstats.size(); i++)
			rval->Set(i, k->list(k->ksr_kstats[i]));
	} catch (Handle<Value> err) {
		return (err);
	}

	return (rval);
}

Handle<Value>
KStatReader::Update(const Arguments& args)
{
	KStatReader *k = ObjectWrap::Unwrap<KStatReader>(args.Holder());
	return (Integer::New(k->update()));
}

Handle<Value>
KStatReader::getKCID(const Arguments& args)
{
	KStatReader *k = ObjectWrap::Unwrap<KStatReader>(args.Holder());
	return (Integer::New(k->getkcid()));
}

/*
 * FIXME check the arguments are of the correct number and type
 */
Handle<Value>
KStatReader::getKstat(const Arguments& args)
{
	KStatReader *k = ObjectWrap::Unwrap<KStatReader>(args.Holder());
	string *imodule = stringMember(args[0], "module", "");
	string module = *imodule;
	int instance = intMember(args[0], "instance", -1);
	string *iname = stringMember(args[0], "name", "");
	string name = *iname;
	kstat_t *ksp = kstat_lookup(k->ksr_ctl, (char *)module.c_str(), instance, (char *)name.c_str());
	if (ksp == NULL) {
		Handle<Object> rval = Object::New();
		rval->Set(String::New("error"), String::New("invalid kstat"));
		rval->Set(String::New("module"), String::New(module.c_str()));
		rval->Set(String::New("instance"), Number::New(instance));
		rval->Set(String::New("name"), String::New(name.c_str()));
		return (rval);
	}
	return (k->read(ksp));
}

extern "C" void
init (Handle<Object> target) 
{
	KStatReader::Initialize(target);
}

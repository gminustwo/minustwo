/* Offline AppContent provider for the configured base-game profile.
 * No package mounting or downloads. Add-ons: GODZILLA's are unlocks whose content is in the base
 * game (pre-order bonuses, early unlocks, diorama figure sets), so every add-on the game asks
 * about is reported as owned, without extra data; MT_UNLOCK_ADDONS=0 reports none instead. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* OrbisAppContentAddcontInfo: entitlement label (16 characters, NUL, 3 padding), status (0 = owned,
 * no extra data). The labels the game asked about are remembered for its list queries. */
#define LABEL_SIZE 17
typedef struct { char label[LABEL_SIZE]; char padding[3]; uint32_t status; } AddonInfo;
_Static_assert(sizeof(AddonInfo)==24,"OrbisAppContentAddcontInfo layout");
static char addon_labels[64][LABEL_SIZE];
static unsigned addon_count;
static int addons_unlocked(void) {
    const char *v=getenv("MT_UNLOCK_ADDONS");
    return !v || strcmp(v,"0")!=0;
}
static void addon_remember(const char *label) {
    for (unsigned i=0;i<addon_count;++i) if (!strncmp(addon_labels[i],label,LABEL_SIZE-1)) return;
    if (addon_count==64) return;
    memcpy(addon_labels[addon_count],label,LABEL_SIZE-1);
    addon_labels[addon_count++][LABEL_SIZE-1]=0;
    printf("Runtime: add-on %s unlocked\n",addon_labels[addon_count-1]);
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* Never touch unused list slots. */
    const unsigned owned=addons_unlocked() ? addon_count : 0;
    unsigned written=0;
    for (; list && written<capacity && written<owned; ++written) {
        AddonInfo info={{0},{0},0};
        memcpy(info.label,addon_labels[written],LABEL_SIZE);
        memcpy((AddonInfo *)list+written,&info,sizeof(info));
    }
    if (hits) *hits=list && capacity ? written : owned;
    ++lists; return 0;
}
/* Without the unlock, the console's answer for an add-on not bought: DRM_NO_ENTITLEMENT. */
static ABI int32_t addon_info(uint32_t service,const char *label,AddonInfo *info) {
    (void)service;
    require_initialized();
    if (!label || !info) return PARAMETER;
    if (!addons_unlocked()) return (int32_t)0x80d90007; /* DRM_NO_ENTITLEMENT */
    addon_remember(label);
    memset(info,0,sizeof(*info));
    memcpy(info->label,label,LABEL_SIZE-1);
    info->status=0; /* NO_EXTRA_DATA: owned, nothing to install */
    return 0;
}
/* The key decrypts an add-on's extra data; these have none, so any key does. */
static ABI int32_t entitlement_key(uint32_t service,const char *label,unsigned char *key) {
    (void)service;
    require_initialized();
    if (!label || !key) return PARAMETER;
    if (!addons_unlocked()) return (int32_t)0x80d90007;
    addon_remember(label);
    memset(key,0,16);
    return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"m47juOmH0VE#libSceAppContent")) return (uintptr_t)addon_info;
    if (!strcmp(name,"XTWR0UXvcgs#libSceAppContent")) return (uintptr_t)entitlement_key;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}

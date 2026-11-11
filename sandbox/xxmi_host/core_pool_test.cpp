// Exercise original pool/expression/callback semantics through command mediation.
#include "draw_plan.h"
#include "layer_gap.h"
#include "HackerContext.h"
#include "globals.h"
#include <cstdio>
void HostParseExcerpt(const wchar_t*,const wchar_t*);
void HostParseConstants();
void HostEnumerateLists();
void HostParseResources();
void HostCompileNamedList(const wchar_t*);
using namespace efmi::frontend;
int HostCorePoolTests()
{
    int passed=0,failed=0;
    auto check=[&](bool ok,const char* message){if(ok)++passed;else{++failed;printf("FAIL: %s\n",message);}};
    registered_command_lists.clear();
    HostParseExcerpt(LR"(
namespace = core_pools
[Constants]
global $index = 1
global $result = 0
[PoolValues]
pool_size = 3
pool_variable_default_value = 5
[PoolAlias]
[PoolResources]
pool_size = 3
[PoolMetadata]
pool_size = 2
pool_variable_default_value = 11
[PoolMixed]
pool_size = 2
pool_element_type_switch_reset = false
pool_variable_default_value = 3
[PoolExpiring]
pool_size = 2
pool_index_type = fifo
pool_expiration_timeout_frames = 1
[ResourceOne]
type = Buffer
stride = 16
[ResourceTwo]
type = Buffer
stride = 8
[ResourceAlias]
[ResourceSnapshot]
[CommandListNative]
[CommandListCallback]
[CommandListBody]
drawindexed = $PoolAlias[$index], 0, 0
[CommandListInitialize]
run = CommandListNative
PoolAlias = ref PoolValues
$PoolValues[$index] = 9
$PoolValues[-1] = 7
$result = $PoolAlias[$index] + $PoolAlias[-1]
ResourceAlias = ref ResourceOne
ResourceSnapshot = ref ResourceAlias
ResourceAlias = ref ResourceTwo
PoolResources[$index] = ref ResourceSnapshot
vb0 = ref PoolResources[$index]
vb1 = ref ResourceAlias
PoolResources[2] = null
vb2 = ref PoolResources[2]
CommandListCallback = ref CommandListBody
run = CommandListCallback
CommandListCallback = null
[CommandListAgain]
run = CommandListCallback
drawindexed = $PoolValues[-1], 0, 0
[CommandListBadIndex]
$index = 0 / 0
$PoolValues[$index] = 2
[CommandListBadRead]
$index = 1 / 0
drawindexed = $PoolValues[$index], 0, 0
[CommandListBadNested]
$result = $PoolValues[@vs-t0]
[CommandListBadCopy]
ResourceAlias = copy ResourceOne
[CommandListBadPoolCycle]
PoolAlias = ref PoolValues
PoolValues = ref PoolAlias
[CommandListBadCallbackCycle]
CommandListCallback = ref CommandListBody
CommandListBody = ref CommandListCallback
[CommandListBadExpiring]
PoolExpiring[$index] = ref ResourceOne
[CommandListCopyMetadata]
$PoolMetadata[0] = 8
PoolMetadata = copy_desc PoolValues
$result = PoolMetadata->size + $PoolMetadata[0]
[CommandListResetVariables]
$PoolMetadata[*] = null
$result = $PoolMetadata[0] + $PoolMetadata[-1]
[CommandListDetachPool]
PoolMetadata = ref PoolValues
PoolMetadata = null
$PoolMetadata[0] = 12
$result = $PoolValues[$index]
[CommandListResetIndex]
PoolValues = null
$result = $PoolValues[$index]
[CommandListIndependentSlots]
PoolResources[0] = ref ResourceOne
PoolResources[1] = ref ResourceTwo
$PoolResources[2] = 4
vb0 = ref PoolResources[0]
vb1 = ref PoolResources[1]
[CommandListSwitchSlot]
PoolResources[0] = ref ResourceOne
$PoolResources[0] = 2
vb0 = ref PoolResources[0]
[CommandListMixed]
PoolMixed[0] = ref ResourceOne
$PoolMixed[0] = 4
vb0 = ref PoolMixed[0]
$result = $PoolMixed[0]
[CommandListResetMixedVariables]
$PoolMixed[*] = null
vb0 = ref PoolMixed[0]
$result = $PoolMixed[0]
[CommandListResetMixedResources]
PoolMixed[*] = null
vb0 = ref PoolMixed[0]
$result = $PoolMixed[0]
[CommandListResetResources]
PoolResources[*] = null
vb0 = ref PoolResources[1]
[CommandListResetAlias]
PoolAlias = ref PoolValues
$PoolAlias[*] = null
$result = $PoolValues[$index]
[CommandListResetLimit]
reset_per_frame_limits = ResourceOne
)",L"core-pools-test.ini");
    HostEnumerateLists();HostParseConstants();HostParseResources();
    const std::wstring prefix=L"commandlist\\core_pools\\";
    DrawPlanAdapter adapter;adapter.external(explicitCommandListSections.at(prefix+L"native"),L"core/init");
    for(auto& pair:explicitCommandListSections)if(pair.first.compare(0,prefix.size(),prefix)==0)HostCompileNamedList(pair.first.c_str());
    optimise_command_lists(nullptr);
    for(auto& pair:explicitCommandListSections)if(pair.first.compare(0,prefix.size(),prefix)==0)adapter.adapt(pair.second.command_list);
    auto run=[&](const wchar_t* name,DrawPlan& plan,std::wstring& reason){
        HackerContext context;CommandListState state;state.mHackerContext=&context;
        return adapter.execute(explicitCommandListSections.at(prefix+name).command_list,state,plan,reason);
    };
    DrawPlan plan;std::wstring reason;
    bool ok=run(L"initialize",plan,reason);
    if(!ok)fwprintf(stderr,L"Core pool test refused: %ls\n",reason.c_str());
    check(ok && command_list_globals.at(L"$\\core_pools\\result").fval==16,"original pool aliases, writes and wrapped reads execute");
    check(ok && plan.items.size()==5 && plan.items[0].operation==PlanOperation::External && plan.items.back().index_count==9,
          "runtime callback executes the adapted source while native initialization remains explicit");
    if(ok && plan.items.size()==5){
        check(plan.items[1].resource==&customResources.at(L"resource\\core_pools\\one") &&
              plan.items[2].resource==&customResources.at(L"resource\\core_pools\\two") && !plan.items[3].resource,
              "resource and pool references snapshot the selected declaration, including null");
    }else check(false,"resource reference snapshots available");
    check(run(L"again",plan,reason) && plan.items.size()==1 && plan.items[0].index_count==7,
          "cleared callback returns to its empty declaration and pools persist across transactions");
    // D-162: each of these has a command that cannot act (an index outside the integers, a cyclic alias, a
    // reference into an expiring pool). It does nothing, the notice names it, and the execution goes on
    // with a plan of its own; each refused the whole execution before.
    static std::vector<std::wstring> notices;notices.clear();
    g_layer_gap_sink=[](const wchar_t* text){notices.emplace_back(text);};
    for(auto* name:{L"badindex",L"badread",L"badpoolcycle",L"badcallbackcycle",L"badexpiring"}){
        command_list_globals.at(L"$\\core_pools\\index").fval=1;
        const size_t before=notices.size();
        check(run(name,plan,reason) && plan.items.empty() && notices.size()>before,
              "a pool, resource or callback operation that cannot act does nothing with a notice, and the execution goes on");
    }
    // A pool index from a slot's identity is answered from the draw state (D-161) and the read goes on.
    command_list_globals.at(L"$\\core_pools\\index").fval=1;
    check(run(L"badnested",plan,reason) && plan.items.empty(),"a pool index read off a slot's identity is answered, and the list goes on");
    g_layer_gap_sink=nullptr;
    // A copy between declared resources the plan does not make leaves the destination null with the
    // original's warning and the list goes on (D-155); it refused the execution before.
    command_list_globals.at(L"$\\core_pools\\index").fval=1;
    check(run(L"badcopy",plan,reason) && customResources.at(L"resource\\core_pools\\alias").is_null,
          "a copy the plan does not make leaves its destination null and the list goes on");
    auto result=[&]{return command_list_globals.at(L"$\\core_pools\\result").fval;};
    check(run(L"copymetadata",plan,reason) && result()==11,
          "copy_desc changes pool size and defaults while preserving existing elements");
    check(run(L"resetvariables",plan,reason) && result()==10,
          "full variable reset uses the copied original pool default");
    check(run(L"detachpool",plan,reason) && result()==9 &&
          customResourcePools.at(L"pool\\core_pools\\metadata").GetVariable(0,false,true,false)->fval==12,
          "pool null detaches a proxy and restores its own elements without changing the source");
    check(run(L"resetindex",plan,reason) && result()==9,
          "whole pool null resets indexing without clearing retained element values");
    check(run(L"independentslots",plan,reason) && plan.items.size()==2 &&
          plan.items[0].resource==&customResources.at(L"resource\\core_pools\\one") &&
          plan.items[1].resource==&customResources.at(L"resource\\core_pools\\two"),
          "scalar writes preserve resource references in unrelated slots of the same pool");
    check(run(L"switchslot",plan,reason) && plan.items.size()==1 && !plan.items[0].resource,
          "switching one resource element to a variable invalidates only that resource");
    check(run(L"mixed",plan,reason) && result()==4 && plan.items.size()==1 &&
          plan.items[0].resource==&customResources.at(L"resource\\core_pools\\one"),
          "mixed pool elements retain independent resource and variable values");
    check(run(L"resetmixedvariables",plan,reason) && result()==3 && plan.items.size()==1 &&
          plan.items[0].resource==&customResources.at(L"resource\\core_pools\\one"),
          "resetting mixed variables preserves the bound resource");
    check(run(L"resetmixedresources",plan,reason) && result()==3 && plan.items.size()==1 && !plan.items[0].resource,
          "resetting mixed resources preserves the variable and removes the binding");
    check(run(L"resetresources",plan,reason) && plan.items.size()==1 && !plan.items[0].resource,
          "full resource reset invalidates the adapter's retained reference");
    check(run(L"resetalias",plan,reason) && result()==5,
          "element reset through a pool alias reaches the original source pool");
    auto& limited=customResources.at(L"resource\\core_pools\\one");
    auto& other=customResources.at(L"resource\\core_pools\\two");
    limited.copies_this_frame=9;other.copies_this_frame=7;
    check(run(L"resetlimit",plan,reason) && limited.copies_this_frame==0 && other.copies_this_frame==7,
          "reset_per_frame_limits resets only the selected original resource counter");
    printf("XXMI Core pools: %d checks passed, %d failed\n",passed,failed);return failed;
}

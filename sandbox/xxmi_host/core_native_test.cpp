#include "draw_plan.h"
#include "resource_plan.h"
#include "../../frontend/core/palette_bank.h"
#include "globals.h"
#include "HackerContext.h"
#include <cstdio>
void HostParseExcerpt(const wchar_t*,const wchar_t*);
void HostParseConstants();
void HostEnumerateLists();
void HostCompileNamedList(const wchar_t*);
using namespace efmi::frontend;
int HostCoreNativeTests()
{
    int passed=0,failed=0;auto check=[&](bool ok,const char* why){if(ok)++passed;else{++failed;printf("FAIL: %s\n",why);}};
    HostParseExcerpt(LR"(
namespace = core_native
[Constants]
global $result = 0
[CommandListRoot]
handling = skip
$result = 0
run = CommandListNative
drawindexed = 3, $result, 0
[CommandListNative]
[CommandListCallback]
$result = $result + 2
post $result = $result + 5
[CommandListBad]
drawindexed = -3, 0, 0
[CommandListUnresolved]
run = CommandListForeign
[CommandListForeign]
[CommandListAlias]
[CommandListRetarget]
CommandListAlias = ref CommandListRetargeted
$result = $result + 2
post $result = $result + 100
[CommandListRetargeted]
post $result = $result + 9
[CommandListPair]
$result = 2
post $result = $result + 3
post drawindexed = 3, $result, 0
[CommandListPairBad]
drawindexed = 3, 0, 0
post drawindexed = -3, 0, 0
[CommandListAbort]
$result = 13
handling = abort
$result = 99
post $result = 100
)",L"core-native.ini");
    HostEnumerateLists();HostParseConstants();
    const std::wstring ns=L"commandlist\\core_native\\";
    auto& root=explicitCommandListSections.at(ns+L"root");
    auto& callback=explicitCommandListSections.at(ns+L"callback");
    auto& bad=explicitCommandListSections.at(ns+L"bad");
    auto& alias=explicitCommandListSections.at(ns+L"alias");
    auto& retarget=explicitCommandListSections.at(ns+L"retarget");
    DrawPlanAdapter adapter;unsigned phase_seen=0;int mode=0;
    auto master=std::make_shared<efmi::core::PaletteSnapshot>();master->identity=37;
    std::weak_ptr<const efmi::core::PaletteSnapshot> weak=master;
    adapter.native(explicitCommandListSections.at(ns+L"native"),L"native/core",
        [&](CommandListState& state,unsigned phases,std::wstring& reason){
            phase_seen|=phases;
            if(mode==1){reason=L"Core input unavailable";return false;}
            if(mode==2){adapter.invoke(bad,state,1,reason);reason.clear();return true;} // Deliberately mishandled by caller.
            if(mode==6){adapter.invoke(bad,state,0,reason);reason.clear();return true;} // An invalid phase, mishandled too.
            if(mode==3)return adapter.invoke(root,state,1,reason);
            if(mode==5)return adapter.invoke(explicitCommandListSections.at(ns+L"abort"),state,3,reason);
            if(mode==4){
                alias.command_list.SetSourceCommandList(&retarget.command_list);
                alias.post_command_list.SetSourceCommandList(&retarget.post_command_list);
                return adapter.invoke(alias,state,3,reason);
            }
            return adapter.invoke(callback,state,3,reason);
        });
    adapter.external(explicitCommandListSections.at(ns+L"foreign"),L"native/foreign");
    bool palette_ready=true;
    adapter.palette_source([&](CommandListState&,std::shared_ptr<const efmi::core::PaletteSnapshot>& out,std::wstring& reason){
        if(!palette_ready){reason=L"Character bones incomplete";return false;}out=master;return true;
    });
    for(auto name:{L"root",L"callback",L"bad",L"unresolved",L"alias",L"retarget",L"retargeted",L"pair",L"pairbad",L"abort"})HostCompileNamedList((ns+name).c_str());
    optimise_command_lists(nullptr);
    for(auto name:{L"root",L"callback",L"bad",L"unresolved",L"alias",L"retarget",L"retargeted",L"pair",L"pairbad",L"abort"}){
        auto& section=explicitCommandListSections.at(ns+name);adapter.adapt(section.command_list);adapter.adapt(section.post_command_list);
    }
    HackerContext context;std::wstring reason;DrawPlan plan;
    auto execute=[&](CommandList& list){CommandListState state;state.mHackerContext=&context;return adapter.execute(list,state,plan,reason);};
    check(execute(root.command_list) && phase_seen && plan.skip_original && plan.items.size()==1 &&
          plan.items[0].operation==PlanOperation::DrawIndexed && plan.items[0].first_index==7,
          "native boundary invokes original callback pre/post in the same transaction");
    check(plan.items[0].palette==master,"native draw context attaches an immutable owned palette");
    auto saved=plan.items[0].palette;
    mode=1;check(!execute(root.command_list) && reason==L"Core input unavailable" && plan.items.size()==1 && plan.items[0].palette==saved,
                 "native refusal preserves the caller's previous complete plan");
    // D-162: a command of the nested callback that cannot act (a draw of a negative count) does nothing and
    // the transaction goes on; it refused the whole transaction before. A failure of the nested call itself
    // (the host's: an invalid phase) still refuses it, also where the native caller ignores it.
    mode=2;check(execute(root.command_list) && plan.items.size()==1 && plan.items[0].first_index==0,
                 "a nested callback's command that cannot act does nothing and the transaction goes on");
    saved=plan.items[0].palette;
    mode=6;check(!execute(root.command_list) && !reason.empty() && plan.items[0].palette==saved,
                 "an ignored failure of the nested call itself still refuses the whole transaction");
    mode=3;check(!execute(root.command_list) && !reason.empty(),"native callback recursion uses the same execution limit");
    mode=4;check(execute(root.command_list) && plan.items[0].first_index==11,
                 "native callback post phase resolves an alias changed during its original pre phase");
    mode=0;palette_ready=false;
    check(!execute(root.command_list) && reason==L"Character bones incomplete" && plan.items[0].palette==saved,
          "missing draw-time bone completeness refuses before publishing any draw");
    palette_ready=true;check(execute(root.command_list) && plan.items[0].first_index==7,"a failed transaction does not poison the next valid execution");
    efmi::assets::FilePlan owned;CustomResources resources;FilePlanCompiler compiler(resources);
    check(compiler.compile(plan,owned,reason) && owned.items[0].palette==master,"owned file plans preserve the native palette owner");
    auto run_pair=[&](const wchar_t* name){
        auto& pair=explicitCommandListSections.at(ns+name);CommandListState state;state.mHackerContext=&context;
        const bool ok=adapter.execute_pair(pair.command_list,pair.post_command_list,state,plan,reason);
        return ok && !state.post;
    };
    check(run_pair(L"pair") && plan.items.size()==1 && plan.items[0].first_index==5,
          "root pre/post execute in one transaction and restore caller phase");
    check(run_pair(L"pairbad") && plan.items.size()==1 && plan.items[0].first_index==0,
          "a root post command that cannot act does nothing and the earlier draw commands are kept");
    mode=5;
    check(execute(root.command_list) && plan.skip_original && plan.items.empty() &&
          command_list_globals.at(L"$\\core_native\\result").fval==13,
          "native callback abort stops its own pre/post lists and the parent draw");
    master.reset();saved.reset();plan={};
    check(!weak.expired() && owned.items[0].palette->identity==37,"compiled native palette data outlives its source plan");
    check(execute(explicitCommandListSections.at(ns+L"unresolved").command_list) && plan.items.size()==1 &&
          plan.items[0].operation==PlanOperation::External && plan.items[0].external==L"native/foreign",
          "unimplemented native boundaries remain explicit unresolved operations");
    CommandListState idle;idle.mHackerContext=&context;
    check(!adapter.invoke(callback,idle,1,reason),"native callbacks cannot execute outside a transaction");
    printf("XXMI native Core bridge: %d checks passed, %d failed\n",passed,failed);return failed;
}

#include "TaskBase.h"

#include "Component/Actuator.h"
#include "Component/Recognizer.h"
#include "Controller/ControllerAgent.h"
#include "Global/GlobalOptionMgr.h"
#include "MaaFramework/MaaMsg.h"
#include "Resource/ResourceMgr.h"
#include "Utils/JsonExt.hpp"
#include "Utils/Logger.h"

MAA_TASK_NS_BEGIN

TaskBase::TaskBase(std::string entry, Tasker* tasker)
    : tasker_(tasker)
    , entry_(std::move(entry))
    , cur_task_(entry_)
    , context_(Context::create(task_id_, tasker))
{
    init();
}

TaskBase::TaskBase(std::string entry, Tasker* tasker, std::shared_ptr<Context> context)
    : tasker_(tasker)
    , entry_(std::move(entry))
    , cur_task_(entry_)
    , context_(std::move(context))
{
    init();
}

bool TaskBase::override_pipeline(const json::value& pipeline_override)
{
    return context_ && context_->override_pipeline(pipeline_override);
}

Tasker* TaskBase::tasker() const
{
    return tasker_;
}

MaaTaskId TaskBase::task_id() const
{
    return task_id_;
}

const std::string& TaskBase::entry() const
{
    return entry_;
}

MAA_RES_NS::ResourceMgr* TaskBase::resource()
{
    return tasker_ ? tasker_->resource() : nullptr;
}

MAA_CTRL_NS::ControllerAgent* TaskBase::controller()
{
    return tasker_ ? tasker_->controller() : nullptr;
}

RecoResult TaskBase::run_recognition(const cv::Mat& image, const PipelineData::NextList& list)
{
    LogFunc << VAR(cur_task_) << VAR(list);

    if (!context_) {
        LogError << "context is null";
        return {};
    }

    if (image.empty()) {
        LogError << "Image is empty";
        return {};
    }

    bool current_focus = context_->get_pipeline_data(cur_task_).focus;

    const json::value reco_list_cb_detail {
        { "task_id", task_id() },
        { "name", cur_task_ },
        { "list", json::array(list) },
    };
    if (debug_mode() || current_focus) {
        notify(MaaMsg_Task_NextList_Starting, reco_list_cb_detail);
    }

    Recognizer recognizer(tasker_, *context_, image);

    for (const auto& name : list) {
        const auto& pipeline_data = context_->get_pipeline_data(name);

        if (!pipeline_data.enabled) {
            LogDebug << "Task disabled" << name << VAR(pipeline_data.enabled);
            continue;
        }

        if (debug_mode() || pipeline_data.focus) {
            const json::value reco_cb_detail {
                { "task_id", task_id() },
                { "reco_id", 0 },
                { "name", name },
            };
            notify(MaaMsg_Task_Recognition_Starting, reco_cb_detail);
        }

        RecoResult result = recognizer.recognize(pipeline_data);

        if (debug_mode() || pipeline_data.focus) {
            const json::value reco_cb_detail {
                { "task_id", task_id() },
                { "reco_id", result.reco_id },
                { "name", name },
            };
            notify(result.box ? MaaMsg_Task_Recognition_Succeeded : MaaMsg_Task_Recognition_Failed, reco_cb_detail);
        }

        if (!result.box) {
            continue;
        }

        LogInfo << "Task hit" << VAR(result.name) << VAR(result.box);

        if (debug_mode() || current_focus) {
            notify(MaaMsg_Task_NextList_Succeeded, reco_list_cb_detail);
        }

        return result;
    }

    if (debug_mode() || current_focus) {
        notify(MaaMsg_Task_NextList_Failed, reco_list_cb_detail);
    }

    return {};
}

NodeDetail TaskBase::run_action(const RecoResult& reco)
{
    if (!context_) {
        LogError << "context is null";
        return {};
    }

    if (!reco.box) {
        LogError << "reco box is nullopt, can NOT run";
        return {};
    }

    const auto& pipeline_data = context_->get_pipeline_data(reco.name);

    if (debug_mode() || pipeline_data.focus) {
        const json::value cb_detail {
            { "task_id", task_id() },
            { "node_id", 0 },
            { "name", reco.name },
        };
        notify(MaaMsg_Task_Action_Starting, cb_detail);
    }
    

    Actuator actuator(tasker_, *context_);
    bool ret = actuator.run(*reco.box, reco.reco_id, pipeline_data);

    NodeDetail result {
        .node_id = generate_node_id(),
        .name = reco.name,
        .reco_id = reco.reco_id,
        .completed = ret,
    };

    set_node_detail(result.node_id, result);

    if (debug_mode() || pipeline_data.focus) {
        const json::value cb_detail {
            { "task_id", task_id() },
            { "node_id", result.node_id },
            { "name", reco.name },
        };
        notify(result.completed ? MaaMsg_Task_Action_Succeeded : MaaMsg_Task_Action_Failed, cb_detail);
    }

    return result;
}

cv::Mat TaskBase::screencap()
{
    if (!controller()) {
        return {};
    }

    return controller()->screencap();
}

MaaTaskId TaskBase::generate_node_id()
{
    return ++s_global_node_id;
}

void TaskBase::set_node_detail(int64_t node_id, NodeDetail detail)
{
    if (!tasker_) {
        LogError << "tasker is null";
        return;
    }

    auto& cache = tasker_->runtime_cache();
    cache.set_node_detail(node_id, detail);
    cache.set_latest_node(detail.name, node_id);

    TaskDetail task_detail = cache.get_task_detail(task_id_).value_or(TaskDetail { .entry = entry_ });
    task_detail.node_ids.emplace_back(node_id);

    set_task_detail(task_detail);
}

void TaskBase::set_task_detail(TaskDetail detail)
{
    if (!tasker_) {
        LogError << "tasker is null";
        return;
    }

    auto& cache = tasker_->runtime_cache();
    cache.set_task_detail(task_id_, detail);
}

void TaskBase::init()
{
    set_task_detail(TaskDetail { .entry = entry_ });
}

bool TaskBase::debug_mode() const
{
    return GlobalOptionMgr::get_instance().debug_mode();
}

void TaskBase::notify(std::string_view msg, const json::value detail)
{
    if (!tasker_) {
        return;
    }

    tasker_->notify(msg, detail);
}

MAA_TASK_NS_END

#include "Goals/goalStore.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <string>
#include <utility>

namespace revia::goals
{

namespace
{

struct DatabaseCloser
{
    void operator()(sqlite3* database) const
    {
        if (database != nullptr)
        {
            sqlite3_close(database);
        }
    }
};

struct StatementCloser
{
    void operator()(sqlite3_stmt* statement) const
    {
        if (statement != nullptr)
        {
            sqlite3_finalize(statement);
        }
    }
};

using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool Execute(sqlite3* database, const char* sql)
{
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

Statement Prepare(sqlite3* database, const char* sql)
{
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    {
        return {};
    }
    return Statement(rawStatement);
}

void BindText(sqlite3_stmt* statement, const int index, const std::string& value)
{
    sqlite3_bind_text(
        statement,
        index,
        value.c_str(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

void BindInt(sqlite3_stmt* statement, const int index, const std::int64_t value)
{
    sqlite3_bind_int64(statement, index, value);
}

std::string ColumnText(sqlite3_stmt* statement, const int column)
{
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
}

std::int64_t ColumnInt(sqlite3_stmt* statement, const int column)
{
    return sqlite3_column_int64(statement, column);
}

std::string EpochSeconds(const std::chrono::system_clock::time_point& value)
{
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        value.time_since_epoch()).count());
}

std::chrono::system_clock::time_point FromEpochSeconds(const std::string& value)
{
    if (value.empty())
    {
        return std::chrono::system_clock::now();
    }
    try
    {
        return std::chrono::system_clock::time_point(std::chrono::seconds(std::stoll(value)));
    }
    catch (const std::exception&)
    {
        return std::chrono::system_clock::now();
    }
}

// Malformed JSON must not abort a load: a single corrupt column degrades to a
// default-constructed payload, and the surrounding status column still tells
// the runner the goal is not safely resumable.
nlohmann::json ParseJson(const std::string& value)
{
    if (value.empty())
    {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
    {
        return nlohmann::json::object();
    }
    return parsed;
}

// The goal store keeps the literal control text of a SetControlText step
// because resuming after a restart has to replay the same action. The action
// audit log still records only its length, so the redaction guarantee there is
// unchanged.
nlohmann::json ActionToJson(const actions::ActionRequest& request)
{
    return nlohmann::json{
        {"id", request.id},
        {"type", actions::ToString(request.type)},
        {"source", actions::PathToUtf8(request.source)},
        {"destination", actions::PathToUtf8(request.destination)},
        {"application", request.application},
        {"window_title", request.windowTitle},
        {"control", request.control},
        {"value", request.value},
        {"dry_run", request.dryRun},
        {"requested_by", request.requestedBy}
    };
}

actions::ActionRequest ActionFromJson(const nlohmann::json& value)
{
    actions::ActionRequest request;
    if (!value.is_object())
    {
        return request;
    }
    request.id = value.value("id", std::string());
    request.type = actions::ActionTypeFromString(value.value("type", std::string()));
    request.source = actions::Utf8ToPath(value.value("source", std::string()));
    request.destination = actions::Utf8ToPath(value.value("destination", std::string()));
    request.application = value.value("application", std::string());
    request.windowTitle = value.value("window_title", std::string());
    request.control = value.value("control", std::string());
    request.value = value.value("value", std::string());
    request.dryRun = value.value("dry_run", false);
    request.requestedBy = value.value("requested_by", std::string("goal"));
    return request;
}

nlohmann::json BudgetToJson(const GoalBudget& budget)
{
    return nlohmann::json{
        {"max_actions", budget.maxActions},
        {"max_retries_per_step", budget.maxRetriesPerStep},
        {"max_total_retries", budget.maxTotalRetries},
        {"max_tokens", budget.maxTokens},
        {"max_duration_ms", budget.maxDurationMs}
    };
}

GoalBudget BudgetFromJson(const nlohmann::json& value)
{
    GoalBudget budget;
    if (!value.is_object())
    {
        return budget;
    }
    budget.maxActions = value.value("max_actions", budget.maxActions);
    budget.maxRetriesPerStep = value.value("max_retries_per_step", budget.maxRetriesPerStep);
    budget.maxTotalRetries = value.value("max_total_retries", budget.maxTotalRetries);
    budget.maxTokens = value.value("max_tokens", budget.maxTokens);
    budget.maxDurationMs = value.value("max_duration_ms", budget.maxDurationMs);
    return budget;
}

nlohmann::json SpendToJson(const GoalSpend& spend)
{
    return nlohmann::json{
        {"actions", spend.actions},
        {"retries", spend.retries},
        {"tokens", spend.tokens},
        {"elapsed_ms", spend.elapsedMs}
    };
}

GoalSpend SpendFromJson(const nlohmann::json& value)
{
    GoalSpend spend;
    if (!value.is_object())
    {
        return spend;
    }
    spend.actions = value.value("actions", spend.actions);
    spend.retries = value.value("retries", spend.retries);
    spend.tokens = value.value("tokens", spend.tokens);
    spend.elapsedMs = value.value("elapsed_ms", spend.elapsedMs);
    return spend;
}

nlohmann::json ScopeToJson(const actions::CapabilitySettings& scope)
{
    nlohmann::json roots = nlohmann::json::array();
    for (const std::filesystem::path& root : scope.approvedRoots)
    {
        roots.push_back(actions::PathToUtf8(root));
    }
    return nlohmann::json{
        {"mode", actions::ToString(scope.mode)},
        {"approved_roots", roots},
        {"approved_applications", scope.approvedApplications},
        {"approved_controls", scope.approvedControls},
        {"auto_approve_risk_through", actions::ToString(scope.autoApproveRiskThrough)},
        {"create_missing_approved_roots", scope.createMissingApprovedRoots},
        {"max_read_bytes", scope.maxReadBytes},
        {"max_directory_entries", scope.maxDirectoryEntries},
        {"max_affected_entries", scope.maxAffectedEntries},
        {"max_desktop_actions_per_minute", scope.maxDesktopActionsPerMinute},
        {"minimum_desktop_action_interval_ms", scope.minimumDesktopActionIntervalMs}
    };
}

// An unreadable scope falls back to CapabilitySettings' own defaults, which are
// supervised mode with no approved roots. That is the narrowest possible scope,
// so a corrupt row can only ever lose authority.
actions::CapabilitySettings ScopeFromJson(const nlohmann::json& value)
{
    actions::CapabilitySettings scope;
    if (!value.is_object())
    {
        return scope;
    }
    scope.mode = actions::ExecutionModeFromString(value.value("mode", std::string()));
    if (value.contains("approved_roots") && value.at("approved_roots").is_array())
    {
        for (const auto& root : value.at("approved_roots"))
        {
            if (root.is_string())
            {
                scope.approvedRoots.push_back(actions::Utf8ToPath(root.get<std::string>()));
            }
        }
    }
    if (value.contains("approved_applications") && value.at("approved_applications").is_array())
    {
        for (const auto& application : value.at("approved_applications"))
        {
            if (application.is_string())
            {
                scope.approvedApplications.push_back(application.get<std::string>());
            }
        }
    }
    if (value.contains("approved_controls") && value.at("approved_controls").is_object())
    {
        for (auto entry = value.at("approved_controls").begin();
             entry != value.at("approved_controls").end(); ++entry)
        {
            if (!entry.value().is_array())
            {
                continue;
            }
            for (const auto& control : entry.value())
            {
                if (control.is_string())
                {
                    scope.approvedControls[entry.key()].push_back(control.get<std::string>());
                }
            }
        }
    }
    scope.autoApproveRiskThrough = actions::RiskLevelFromString(
        value.value("auto_approve_risk_through", std::string()));
    scope.createMissingApprovedRoots =
        value.value("create_missing_approved_roots", scope.createMissingApprovedRoots);
    scope.maxReadBytes = value.value("max_read_bytes", scope.maxReadBytes);
    scope.maxDirectoryEntries = value.value("max_directory_entries", scope.maxDirectoryEntries);
    scope.maxAffectedEntries = value.value("max_affected_entries", scope.maxAffectedEntries);
    scope.maxDesktopActionsPerMinute = value.value(
        "max_desktop_actions_per_minute",
        scope.maxDesktopActionsPerMinute);
    scope.minimumDesktopActionIntervalMs = value.value(
        "minimum_desktop_action_interval_ms",
        scope.minimumDesktopActionIntervalMs);
    return scope;
}

Database OpenDatabase(const std::string& storePath)
{
    const std::filesystem::path path(storePath);
    std::error_code directoryError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError)
        {
            return {};
        }
    }

    sqlite3* rawDatabase = nullptr;
    if (sqlite3_open_v2(
        storePath.c_str(),
        &rawDatabase,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr) != SQLITE_OK)
    {
        if (rawDatabase != nullptr)
        {
            sqlite3_close(rawDatabase);
        }
        return {};
    }

    Database database(rawDatabase);
    sqlite3_busy_timeout(database.get(), 2000);
    constexpr const char* Schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS goals ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  stop_reason TEXT NOT NULL,"
        "  current_step INTEGER NOT NULL DEFAULT 0,"
        "  budget TEXT NOT NULL,"
        "  spend TEXT NOT NULL,"
        "  scope TEXT NOT NULL,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS goals_by_status ON goals(status, updated_at);"
        "CREATE TABLE IF NOT EXISTS goal_steps ("
        "  id TEXT PRIMARY KEY,"
        "  goal_id TEXT NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  description TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  check_action TEXT NOT NULL,"
        "  expected TEXT NOT NULL,"
        "  FOREIGN KEY(goal_id) REFERENCES goals(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS goal_steps_by_goal ON goal_steps(goal_id, ordinal);"
        "CREATE TABLE IF NOT EXISTS goal_attempts ("
        "  step_id TEXT NOT NULL,"
        "  attempt INTEGER NOT NULL,"
        "  action_id TEXT NOT NULL,"
        "  check_action_id TEXT NOT NULL,"
        "  verdict TEXT NOT NULL,"
        "  executed INTEGER NOT NULL DEFAULT 0,"
        "  verified INTEGER NOT NULL DEFAULT 0,"
        "  observation TEXT NOT NULL,"
        "  failure TEXT NOT NULL,"
        "  occurred_at TEXT NOT NULL,"
        "  PRIMARY KEY(step_id, attempt),"
        "  FOREIGN KEY(step_id) REFERENCES goal_steps(id) ON DELETE CASCADE"
        ");";
    if (!Execute(database.get(), Schema))
    {
        return {};
    }
    return database;
}

bool WriteAttempt(sqlite3* database, const std::string& stepId, const StepAttempt& attempt)
{
    Statement insert = Prepare(database,
        "INSERT OR REPLACE INTO goal_attempts "
        "(step_id, attempt, action_id, check_action_id, verdict, executed, verified, "
        " observation, failure, occurred_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!insert)
    {
        return false;
    }

    BindText(insert.get(), 1, stepId);
    BindInt(insert.get(), 2, attempt.attempt);
    BindText(insert.get(), 3, attempt.actionId);
    BindText(insert.get(), 4, attempt.checkActionId);
    BindText(insert.get(), 5, actions::ToString(attempt.verdict));
    BindInt(insert.get(), 6, attempt.executed ? 1 : 0);
    BindInt(insert.get(), 7, attempt.verified ? 1 : 0);
    BindText(insert.get(), 8, attempt.observation);
    BindText(insert.get(), 9, attempt.failure);
    BindText(insert.get(), 10, EpochSeconds(attempt.occurredAt));
    return sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool WriteStep(sqlite3* database, const std::string& goalId, const GoalStep& step)
{
    Statement insert = Prepare(database,
        "INSERT OR REPLACE INTO goal_steps "
        "(id, goal_id, ordinal, description, status, action, check_action, expected) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
    if (!insert)
    {
        return false;
    }

    BindText(insert.get(), 1, step.id);
    BindText(insert.get(), 2, goalId);
    BindInt(insert.get(), 3, step.ordinal);
    BindText(insert.get(), 4, step.description);
    BindText(insert.get(), 5, ToString(step.status));
    BindText(insert.get(), 6, ActionToJson(step.action).dump());
    BindText(insert.get(), 7, ActionToJson(step.check).dump());
    BindText(insert.get(), 8, step.expected);
    if (sqlite3_step(insert.get()) != SQLITE_DONE)
    {
        return false;
    }

    for (const StepAttempt& attempt : step.attempts)
    {
        if (!WriteAttempt(database, step.id, attempt))
        {
            return false;
        }
    }
    return true;
}

bool WriteGoal(sqlite3* database, const Goal& goal)
{
    Statement insert = Prepare(database,
        "INSERT OR REPLACE INTO goals "
        "(id, title, status, stop_reason, current_step, budget, spend, scope, "
        " created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!insert)
    {
        return false;
    }

    BindText(insert.get(), 1, goal.id);
    BindText(insert.get(), 2, goal.title);
    BindText(insert.get(), 3, ToString(goal.status));
    BindText(insert.get(), 4, ToString(goal.stopReason));
    BindInt(insert.get(), 5, goal.currentStep);
    BindText(insert.get(), 6, BudgetToJson(goal.budget).dump());
    BindText(insert.get(), 7, SpendToJson(goal.spend).dump());
    BindText(insert.get(), 8, ScopeToJson(goal.scope).dump());
    BindText(insert.get(), 9, EpochSeconds(goal.createdAt));
    BindText(insert.get(), 10, EpochSeconds(goal.updatedAt));
    if (sqlite3_step(insert.get()) != SQLITE_DONE)
    {
        return false;
    }

    // Steps are rewritten wholesale rather than diffed. A goal owns a bounded
    // number of steps, and replacing them keeps the stored plan identical to
    // the in-memory plan instead of merging two partial views of it.
    Statement clear = Prepare(database, "DELETE FROM goal_steps WHERE goal_id = ?;");
    if (!clear)
    {
        return false;
    }
    BindText(clear.get(), 1, goal.id);
    if (sqlite3_step(clear.get()) != SQLITE_DONE)
    {
        return false;
    }

    for (const GoalStep& step : goal.steps)
    {
        if (!WriteStep(database, goal.id, step))
        {
            return false;
        }
    }
    return true;
}

std::vector<StepAttempt> ReadAttempts(sqlite3* database, const std::string& stepId)
{
    std::vector<StepAttempt> attempts;
    Statement select = Prepare(database,
        "SELECT attempt, action_id, check_action_id, verdict, executed, verified, "
        "       observation, failure, occurred_at "
        "FROM goal_attempts WHERE step_id = ? ORDER BY attempt ASC;");
    if (!select)
    {
        return attempts;
    }

    BindText(select.get(), 1, stepId);
    while (sqlite3_step(select.get()) == SQLITE_ROW)
    {
        StepAttempt attempt;
        attempt.attempt = static_cast<std::uint32_t>(ColumnInt(select.get(), 0));
        attempt.actionId = ColumnText(select.get(), 1);
        attempt.checkActionId = ColumnText(select.get(), 2);
        attempt.verdict = ColumnText(select.get(), 3) == "allowed"
            ? actions::PolicyVerdict::Allowed
            : (ColumnText(select.get(), 3) == "requires_confirmation"
                ? actions::PolicyVerdict::RequiresConfirmation
                : actions::PolicyVerdict::Blocked);
        attempt.executed = ColumnInt(select.get(), 4) != 0;
        attempt.verified = ColumnInt(select.get(), 5) != 0;
        attempt.observation = ColumnText(select.get(), 6);
        attempt.failure = ColumnText(select.get(), 7);
        attempt.occurredAt = FromEpochSeconds(ColumnText(select.get(), 8));
        attempts.push_back(std::move(attempt));
    }
    return attempts;
}

std::vector<GoalStep> ReadSteps(sqlite3* database, const std::string& goalId)
{
    std::vector<GoalStep> steps;
    Statement select = Prepare(database,
        "SELECT id, ordinal, description, status, action, check_action, expected "
        "FROM goal_steps WHERE goal_id = ? ORDER BY ordinal ASC;");
    if (!select)
    {
        return steps;
    }

    BindText(select.get(), 1, goalId);
    while (sqlite3_step(select.get()) == SQLITE_ROW)
    {
        GoalStep step;
        step.id = ColumnText(select.get(), 0);
        step.ordinal = static_cast<std::uint32_t>(ColumnInt(select.get(), 1));
        step.description = ColumnText(select.get(), 2);
        step.status = StepStatusFromString(ColumnText(select.get(), 3));
        step.action = ActionFromJson(ParseJson(ColumnText(select.get(), 4)));
        step.check = ActionFromJson(ParseJson(ColumnText(select.get(), 5)));
        step.expected = ColumnText(select.get(), 6);
        steps.push_back(std::move(step));
    }

    for (GoalStep& step : steps)
    {
        step.attempts = ReadAttempts(database, step.id);
    }
    return steps;
}

Goal ReadGoal(sqlite3* database, sqlite3_stmt* statement)
{
    Goal goal;
    goal.id = ColumnText(statement, 0);
    goal.title = ColumnText(statement, 1);
    goal.status = GoalStatusFromString(ColumnText(statement, 2));
    goal.stopReason = StopReasonFromString(ColumnText(statement, 3));
    goal.currentStep = static_cast<std::uint32_t>(ColumnInt(statement, 4));
    goal.budget = BudgetFromJson(ParseJson(ColumnText(statement, 5)));
    goal.spend = SpendFromJson(ParseJson(ColumnText(statement, 6)));
    goal.scope = ScopeFromJson(ParseJson(ColumnText(statement, 7)));
    goal.createdAt = FromEpochSeconds(ColumnText(statement, 8));
    goal.updatedAt = FromEpochSeconds(ColumnText(statement, 9));
    goal.steps = ReadSteps(database, goal.id);
    return goal;
}

constexpr const char* GoalColumns =
    "SELECT id, title, status, stop_reason, current_step, budget, spend, scope, "
    "       created_at, updated_at FROM goals ";

} // namespace

GoalStore::GoalStore(std::string path)
    : storePath(std::move(path))
{
}

bool GoalStore::Save(const Goal& goal) const
{
    if (goal.id.empty())
    {
        return false;
    }

    Database database = OpenDatabase(storePath);
    if (!database)
    {
        return false;
    }

    if (!Execute(database.get(), "BEGIN IMMEDIATE;"))
    {
        return false;
    }
    if (!WriteGoal(database.get(), goal))
    {
        static_cast<void>(Execute(database.get(), "ROLLBACK;"));
        return false;
    }
    return Execute(database.get(), "COMMIT;");
}

std::optional<Goal> GoalStore::Load(const std::string& goalId) const
{
    if (goalId.empty())
    {
        return std::nullopt;
    }

    Database database = OpenDatabase(storePath);
    if (!database)
    {
        return std::nullopt;
    }

    Statement select = Prepare(database.get(),
        (std::string(GoalColumns) + "WHERE id = ?;").c_str());
    if (!select)
    {
        return std::nullopt;
    }

    BindText(select.get(), 1, goalId);
    if (sqlite3_step(select.get()) != SQLITE_ROW)
    {
        return std::nullopt;
    }
    return ReadGoal(database.get(), select.get());
}

std::vector<Goal> GoalStore::LoadResumable() const
{
    std::vector<Goal> goals;
    Database database = OpenDatabase(storePath);
    if (!database)
    {
        return goals;
    }

    Statement select = Prepare(database.get(),
        (std::string(GoalColumns) +
         "WHERE status IN ('planned','running','blocked') ORDER BY updated_at ASC;").c_str());
    if (!select)
    {
        return goals;
    }

    while (sqlite3_step(select.get()) == SQLITE_ROW)
    {
        goals.push_back(ReadGoal(database.get(), select.get()));
    }
    return goals;
}

std::vector<Goal> GoalStore::LoadRecent(const std::size_t maxGoals) const
{
    std::vector<Goal> goals;
    if (maxGoals == 0)
    {
        return goals;
    }

    Database database = OpenDatabase(storePath);
    if (!database)
    {
        return goals;
    }

    Statement select = Prepare(database.get(),
        (std::string(GoalColumns) + "ORDER BY updated_at DESC LIMIT ?;").c_str());
    if (!select)
    {
        return goals;
    }

    BindInt(select.get(), 1, static_cast<std::int64_t>(maxGoals));
    while (sqlite3_step(select.get()) == SQLITE_ROW)
    {
        goals.push_back(ReadGoal(database.get(), select.get()));
    }
    return goals;
}

bool GoalStore::Remove(const std::string& goalId) const
{
    if (goalId.empty())
    {
        return false;
    }

    Database database = OpenDatabase(storePath);
    if (!database)
    {
        return false;
    }

    Statement remove = Prepare(database.get(), "DELETE FROM goals WHERE id = ?;");
    if (!remove)
    {
        return false;
    }
    BindText(remove.get(), 1, goalId);
    return sqlite3_step(remove.get()) == SQLITE_DONE;
}

const std::string& GoalStore::Path() const
{
    return storePath;
}

} // namespace revia::goals

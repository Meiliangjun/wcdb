/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <WCDB/Assertion.hpp>
#include <WCDB/CoreConst.h>
#include <WCDB/MigrateHandle.hpp>
#include <WCDB/Time.hpp>
#include <cmath>

namespace WCDB {

MigrateHandle::MigrateHandle()
: m_migratingInfo(nullptr)
, m_migrateStatement(getStatement())
, m_removeMigratedStatement(getStatement())
, m_samplePointing(0)
{
    m_error.infos.insert_or_assign(ErrorStringKeyAction, ErrorActionMigrate);
}

MigrateHandle::~MigrateHandle()
{
    finalizeMigrationStatement();
    returnStatement(m_migrateStatement);
    returnStatement(m_removeMigratedStatement);
}

bool MigrateHandle::reAttach(const UnsafeStringView& newPath, const Schema& newSchema)
{
    WCTInnerAssert(!isInTransaction());
    WCTInnerAssert(!isPrepared());

    bool succeed = true;
    if (!m_attached.syntax().isTargetingSameSchema(newSchema.syntax())) {
        succeed = detach() && attach(newPath, newSchema);
    }
    m_migratingInfo = nullptr;
    finalizeMigrationStatement();
    return succeed;
}

bool MigrateHandle::attach(const UnsafeStringView& newPath, const Schema& newSchema)
{
    WCTInnerAssert(!isInTransaction());
    WCTInnerAssert(!isPrepared());
    WCTInnerAssert(m_attached.syntax().isMain());

    bool succeed = true;
    if (!newSchema.syntax().isMain()) {
        succeed = execute(WCDB::StatementAttach().attach(newPath).as(newSchema));
        if (succeed) {
            m_attached = newSchema;
        }
    }
    return succeed;
}

bool MigrateHandle::detach()
{
    WCTInnerAssert(!isInTransaction());
    WCTInnerAssert(!isPrepared());

    bool succeed = true;
    if (!m_attached.syntax().isMain()) {
        succeed = execute(WCDB::StatementDetach().detach(m_attached));
        if (succeed) {
            m_attached = Schema::main();
        }
    }
    return succeed;
}

#pragma mark - Stepper
std::pair<bool, std::set<StringView>> MigrateHandle::getAllTables()
{
    Column name("name");
    Column type("type");
    StringView pattern = StringView::formatted("%s%%", Syntax::builtinTablePrefix);
    return getValues(StatementSelect()
                     .select(name)
                     .from(TableOrSubquery::master())
                     .where(type == "table" && name.notLike(pattern)),
                     0);
}

bool MigrateHandle::dropSourceTable(const MigrationInfo* info)
{
    WCTInnerAssert(info != nullptr);
    bool succeed = false;
    if (reAttach(info->getSourceDatabase(), info->getSchemaForSourceDatabase())) {
        m_migratingInfo = info;
        succeed = execute(m_migratingInfo->getStatementForDroppingSourceTable());
    }
    return succeed;
}

bool MigrateHandle::migrateRows(const MigrationInfo* info, bool& done)
{
    WCTInnerAssert(info != nullptr);
    done = false;

    bool succeed, exists;
    std::tie(succeed, exists) = tableExists(info->getTable());
    if (!succeed) {
        return false;
    }

    if (!exists) {
        done = true;
        return true;
    }

    if (m_migratingInfo != info) {
        if (!reAttach(info->getSourceDatabase(), info->getSchemaForSourceDatabase())) {
            return false;
        }
        m_migratingInfo = info;
    }

    if (!m_migrateStatement->isPrepared()
        && !m_migrateStatement->prepare(m_migratingInfo->getStatementForMigratingOneRow())) {
        return false;
    }

    if (!m_removeMigratedStatement->isPrepared()
        && !m_removeMigratedStatement->prepare(
           m_migratingInfo->getStatementForDeletingMigratedOneRow())) {
        return false;
    }

    double timeIntervalWithinTransaction = calculateTimeIntervalWithinTransaction();
    SteadyClock beforeTransaction = SteadyClock::now();
    bool migrated = false;
    succeed = runTransaction(
    [&migrated, &beforeTransaction, &timeIntervalWithinTransaction, this](Handle*) -> bool {
        bool succeed = false;
        double cost = 0;
        do {
            std::tie(succeed, migrated) = migrateRow();
            cost = SteadyClock::timeIntervalSinceSteadyClockToNow(beforeTransaction);
        } while (succeed && !migrated && cost < timeIntervalWithinTransaction);
        timeIntervalWithinTransaction = cost;
        return succeed;
    });
    if (succeed) {
        // update only if succeed
        double timeIntervalWholeTranscation
        = SteadyClock::timeIntervalSinceSteadyClockToNow(beforeTransaction);
        addSample(timeIntervalWithinTransaction, timeIntervalWholeTranscation);

        if (migrated) {
            done = true;
        }
    }
    return succeed;
}

std::pair<bool, bool> MigrateHandle::migrateRow()
{
    WCTInnerAssert(m_migrateStatement->isPrepared()
                   && m_removeMigratedStatement->isPrepared());
    WCTInnerAssert(isInTransaction());
    bool succeed = false;
    bool migrated = false;
    m_migrateStatement->reset();
    m_removeMigratedStatement->reset();
    if (m_migrateStatement->step()) {
        if (getChanges() != 0) {
            succeed = m_removeMigratedStatement->step();
        } else {
            succeed = true;
            migrated = true;
        }
    }
    return { succeed, migrated };
}

void MigrateHandle::finalizeMigrationStatement()
{
    m_migrateStatement->finalize();
    m_removeMigratedStatement->finalize();
}

#pragma mark - Sample
MigrateHandle::Sample::Sample()
: timeIntervalWithinTransaction(0), timeIntervalWholeTransaction(0)
{
}

void MigrateHandle::addSample(double timeIntervalWithinTransaction, double timeIntervalForWholeTransaction)
{
    WCTInnerAssert(timeIntervalWithinTransaction > 0);
    WCTInnerAssert(timeIntervalForWholeTransaction > 0);
    WCTInnerAssert(m_samplePointing < numberOfSamples);
    WCTInnerAssert(timeIntervalForWholeTransaction > timeIntervalWithinTransaction);

    Sample& sample = m_samples[m_samplePointing];
    sample.timeIntervalWithinTransaction = timeIntervalWithinTransaction;
    sample.timeIntervalWholeTransaction = timeIntervalForWholeTransaction;
    ++m_samplePointing;
    if (m_samplePointing >= numberOfSamples) {
        m_samplePointing = 0;
    }
}

double MigrateHandle::calculateTimeIntervalWithinTransaction() const
{
    double totalTimeIntervalWithinTransaction = 0;
    double totalTimeIntervalWholeTransaction = 0;
    for (const auto& sample : m_samples) {
        if (sample.timeIntervalWithinTransaction > 0
            && sample.timeIntervalWholeTransaction > 0) {
            totalTimeIntervalWithinTransaction += sample.timeIntervalWithinTransaction;
            totalTimeIntervalWholeTransaction += sample.timeIntervalWholeTransaction;
        }
    }
    double timeIntervalWithinTransaction = MigrateMaxExpectingDuration * totalTimeIntervalWithinTransaction
                                           / totalTimeIntervalWholeTransaction;
    if (timeIntervalWithinTransaction > MigrateMaxExpectingDuration
        || timeIntervalWithinTransaction <= 0 || std::isnan(timeIntervalWithinTransaction)) {
        timeIntervalWithinTransaction = MigrateMaxInitializeDuration;
    }
    return timeIntervalWithinTransaction;
}

#pragma mark - Info Initializer
std::pair<bool, bool> MigrateHandle::sourceTableExists(const MigrationUserInfo& userInfo)
{
    bool succeed = false;
    bool exists = false;
    do {
        Schema schema = userInfo.getSchemaForSourceDatabase();
        if (!reAttach(userInfo.getSourceDatabase(), schema)) {
            break;
        }
        std::tie(succeed, exists) = tableExists(schema, userInfo.getSourceTable());
    } while (false);
    return { succeed, exists };
}

std::tuple<bool, bool, std::set<StringView>>
MigrateHandle::getColumnsOfUserInfo(const MigrationUserInfo& userInfo)
{
    bool succeed = true;
    bool integerPrimary = false;
    std::set<StringView> columns;
    do {
        bool exists;
        std::tie(succeed, exists) = tableExists(Schema::main(), userInfo.getTable());
        if (!succeed) {
            break;
        }
        if (exists) {
            std::vector<ColumnMeta> columnMetas;
            std::tie(succeed, columnMetas)
            = getTableMeta(Schema::main(), userInfo.getTable());
            if (succeed) {
                integerPrimary = ColumnMeta::getIndexOfIntegerPrimary(columnMetas) >= 0;
                for (const auto& columnMeta : columnMetas) {
                    columns.emplace(columnMeta.name);
                }
            }
        }
    } while (false);
    return { succeed, integerPrimary, columns };
}

StringView MigrateHandle::getDatabasePath() const
{
    return getPath();
}

} // namespace WCDB

/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qbs.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "language.h"

#include "artifactproperties.h"
#include "propertymapinternal.h"
#include "resolvedfilecontext.h"
#include "scriptengine.h"

#include <buildgraph/artifact.h>
#include <buildgraph/productbuilddata.h>
#include <buildgraph/projectbuilddata.h>
#include <buildgraph/rulegraph.h> // TODO: Move to language?
#include <buildgraph/transformer.h>
#include <jsextensions/jsextensions.h>
#include <logging/translator.h>
#include <tools/buildgraphlocker.h>
#include <tools/hostosinfo.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/persistence.h>
#include <tools/scripttools.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>

#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qmap.h>

#include <QtScript/qscriptvalue.h>

#include <algorithm>
#include <mutex>

namespace qbs {
namespace Internal {

template<typename T> bool equals(const T *v1, const T *v2)
{
    if (v1 == v2)
        return true;
    if (!v1 != !v2)
        return false;
    return *v1 == *v2;
}


FileTagger::FileTagger(const QStringList &patterns, const FileTags &fileTags)
    : m_fileTags(fileTags)
{
    setPatterns(patterns);
}

void FileTagger::setPatterns(const QStringList &patterns)
{
    m_patterns.clear();
    for (const QString &pattern : patterns) {
        QBS_CHECK(!pattern.isEmpty());
        m_patterns << QRegExp(pattern, Qt::CaseSensitive, QRegExp::Wildcard);
    }
}

/*!
 * \class FileTagger
 * \brief The \c FileTagger class maps 1:1 to the respective item in a qbs source file.
 */
void FileTagger::load(PersistentPool &pool)
{
    setPatterns(pool.load<QStringList>());
    pool.load(m_fileTags);
}

void FileTagger::store(PersistentPool &pool) const
{
    QStringList patterns;
    for (const QRegExp &regExp : qAsConst(m_patterns))
        patterns << regExp.pattern();
    pool.store(patterns);
    pool.store(m_fileTags);
}


void Probe::load(PersistentPool &pool)
{
    pool.load(m_globalId);
    m_location.load(pool);
    pool.load(m_condition);
    pool.load(m_configureScript);
    pool.load(m_properties);
    pool.load(m_initialProperties);
}

void Probe::store(PersistentPool &pool) const
{
    pool.store(m_globalId);
    m_location.store(pool);
    pool.store(condition());
    pool.store(m_configureScript);
    pool.store(m_properties);
    pool.store(m_initialProperties);
}


/*!
 * \class SourceArtifact
 * \brief The \c SourceArtifact class represents a source file.
 * Everything except the file path is inherited from the surrounding \c ResolvedGroup.
 * (TODO: Not quite true. Artifacts in transformers will be generated by the transformer, but are
 * still represented as source artifacts. We may or may not want to change this; if we do,
 * SourceArtifact could simply have a back pointer to the group in addition to the file path.)
 * \sa ResolvedGroup
 */
void SourceArtifactInternal::load(PersistentPool &pool)
{
    pool.load(absoluteFilePath);
    pool.load(fileTags);
    pool.load(overrideFileTags);
    pool.load(properties);
}

void SourceArtifactInternal::store(PersistentPool &pool) const
{
    pool.store(absoluteFilePath);
    pool.store(fileTags);
    pool.store(overrideFileTags);
    pool.store(properties);
}

void SourceWildCards::load(PersistentPool &pool)
{
    pool.load(prefix);
    pool.load(patterns);
    pool.load(excludePatterns);
    pool.load(dirTimeStamps);
    pool.load(files);
}

void SourceWildCards::store(PersistentPool &pool) const
{
    pool.store(prefix);
    pool.store(patterns);
    pool.store(excludePatterns);
    pool.store(dirTimeStamps);
    pool.store(files);
}

/*!
 * \class ResolvedGroup
 * \brief The \c ResolvedGroup class corresponds to the Group item in a qbs source file.
 */

 /*!
  * \variable ResolvedGroup::files
  * \brief The files listed in the group item's "files" binding.
  * Note that these do not include expanded wildcards.
  */

/*!
 * \variable ResolvedGroup::wildcards
 * \brief Represents the wildcard elements in this group's "files" binding.
 *  If no wildcards are specified there, this variable is null.
 * \sa SourceWildCards
 */

/*!
 * \brief Returns all files specified in the group item as source artifacts.
 * This includes the expanded list of wildcards.
 */
QList<SourceArtifactPtr> ResolvedGroup::allFiles() const
{
    QList<SourceArtifactPtr> lst = files;
    if (wildcards)
        lst.append(wildcards->files);
    return lst;
}

void ResolvedGroup::load(PersistentPool &pool)
{
    pool.load(name);
    pool.load(enabled);
    pool.load(location);
    pool.load(prefix);
    pool.load(files);
    pool.load(wildcards);
    pool.load(properties);
    pool.load(fileTags);
    pool.load(overrideTags);
}

void ResolvedGroup::store(PersistentPool &pool) const
{
    pool.store(name);
    pool.store(enabled);
    pool.store(location);
    pool.store(prefix);
    pool.store(files);
    pool.store(wildcards);
    pool.store(properties);
    pool.store(fileTags);
    pool.store(overrideTags);
}

/*!
 * \class RuleArtifact
 * \brief The \c RuleArtifact class represents an Artifact item encountered in the context
 *        of a Rule item.
 * When applying the rule, one \c Artifact object will be constructed from each \c RuleArtifact
 * object. During that process, the \c RuleArtifact's bindings are evaluated and the results
 * are inserted into the corresponding \c Artifact's properties.
 * \sa Rule
 */

void RuleArtifact::Binding::store(PersistentPool &pool) const
{
    pool.store(name);
    pool.store(code);
    pool.store(location);
}

void RuleArtifact::Binding::load(PersistentPool &pool)
{
    pool.load(name);
    pool.load(code);
    pool.load(location);
}

void RuleArtifact::load(PersistentPool &pool)
{
    pool.load(filePath);
    pool.load(fileTags);
    pool.load(alwaysUpdated);
    pool.load(location);
    pool.load(filePathLocation);
    pool.load(bindings);
}

void RuleArtifact::store(PersistentPool &pool) const
{
    pool.store(filePath);
    pool.store(fileTags);
    pool.store(alwaysUpdated);
    pool.store(location);
    pool.store(filePathLocation);
    pool.store(bindings);
}


/*!
 * \class ScriptFunction
 * \brief The \c ScriptFunction class represents the JavaScript code found in the "prepare" binding
 *        of a \c Rule item in a qbs file.
 * \sa Rule
 */

ScriptFunction::ScriptFunction()
{

}

ScriptFunction::~ScriptFunction()
{

}

 /*!
  * \variable ScriptFunction::script
  * \brief The actual Javascript code, taken verbatim from the qbs source file.
  */

  /*!
   * \variable ScriptFunction::location
   * \brief The exact location of the script in the qbs source file.
   * This is mostly needed for diagnostics.
   */

bool ScriptFunction::isValid() const
{
    return location.line() != -1;
}

void ScriptFunction::load(PersistentPool &pool)
{
    pool.load(sourceCode);
    pool.load(argumentNames);
    location.load(pool);
    pool.load(fileContext);
}

void ScriptFunction::store(PersistentPool &pool) const
{
    pool.store(sourceCode);
    pool.store(argumentNames);
    location.store(pool);
    pool.store(fileContext);
}

bool operator==(const ScriptFunction &a, const ScriptFunction &b)
{
    return a.sourceCode == b.sourceCode
            && a.location == b.location
            && a.argumentNames == b.argumentNames
            && equals(a.fileContext.data(), b.fileContext.data());
}

void ResolvedModule::load(PersistentPool &pool)
{
    pool.load(name);
    pool.load(moduleDependencies);
    pool.load(setupBuildEnvironmentScript);
    pool.load(setupRunEnvironmentScript);
}

void ResolvedModule::store(PersistentPool &pool) const
{
    pool.store(name);
    pool.store(moduleDependencies);
    pool.store(setupBuildEnvironmentScript);
    pool.store(setupRunEnvironmentScript);
}

bool operator==(const ResolvedModule &m1, const ResolvedModule &m2)
{
    return m1.name == m2.name
            && m1.moduleDependencies.toSet() == m2.moduleDependencies.toSet()
            && equals(m1.setupBuildEnvironmentScript.data(), m2.setupBuildEnvironmentScript.data())
            && equals(m1.setupRunEnvironmentScript.data(), m2.setupRunEnvironmentScript.data());
}

QString Rule::toString() const
{
    QStringList outputTagsSorted = collectedOutputFileTags().toStringList();
    outputTagsSorted.sort();
    FileTags inputTags = inputs;
    inputTags.unite(inputsFromDependencies);
    QStringList inputTagsSorted = inputTags.toStringList();
    inputTagsSorted.sort();
    return QLatin1Char('[') + outputTagsSorted.join(QLatin1Char(','))
            + QLatin1String("][")
            + inputTagsSorted.join(QLatin1Char(',')) + QLatin1Char(']');
}

bool Rule::acceptsAsInput(Artifact *artifact) const
{
    return artifact->fileTags().intersects(inputs);
}

FileTags Rule::staticOutputFileTags() const
{
    FileTags result;
    for (const RuleArtifactConstPtr &artifact : qAsConst(artifacts))
        result.unite(artifact->fileTags);
    return result;
}

FileTags Rule::collectedOutputFileTags() const
{
    return outputFileTags.isEmpty() ? staticOutputFileTags() : outputFileTags;
}

bool Rule::isDynamic() const
{
    return outputArtifactsScript->isValid();
}

bool Rule::requiresInputs() const
{
    return !inputs.isEmpty() || !inputsFromDependencies.isEmpty();
}

void Rule::load(PersistentPool &pool)
{
    pool.load(name);
    pool.load(prepareScript);
    pool.load(outputArtifactsScript);
    pool.load(module);
    pool.load(inputs);
    pool.load(outputFileTags);
    pool.load(auxiliaryInputs);
    pool.load(excludedAuxiliaryInputs);
    pool.load(inputsFromDependencies);
    pool.load(explicitlyDependsOn);
    pool.load(multiplex);
    pool.load(alwaysRun);
    pool.load(artifacts);
}

void Rule::store(PersistentPool &pool) const
{
    pool.store(name);
    pool.store(prepareScript);
    pool.store(outputArtifactsScript);
    pool.store(module);
    pool.store(inputs);
    pool.store(outputFileTags);
    pool.store(auxiliaryInputs);
    pool.store(excludedAuxiliaryInputs);
    pool.store(inputsFromDependencies);
    pool.store(explicitlyDependsOn);
    pool.store(multiplex);
    pool.store(alwaysRun);
    pool.store(artifacts);
}

ResolvedProduct::ResolvedProduct()
    : enabled(true)
{
}

ResolvedProduct::~ResolvedProduct()
{
}

void ResolvedProduct::accept(BuildGraphVisitor *visitor) const
{
    if (!buildData)
        return;
    for (BuildGraphNode * const node : qAsConst(buildData->roots))
        node->accept(visitor);
}

/*!
 * \brief Returns all files of all groups as source artifacts.
 * This includes the expanded list of wildcards.
 */
QList<SourceArtifactPtr> ResolvedProduct::allFiles() const
{
    QList<SourceArtifactPtr> lst;
    for (const GroupConstPtr &group : qAsConst(groups))
        lst += group->allFiles();
    return lst;
}

/*!
 * \brief Returns all files of all enabled groups as source artifacts.
 * \sa ResolvedProduct::allFiles()
 */
QList<SourceArtifactPtr> ResolvedProduct::allEnabledFiles() const
{
    QList<SourceArtifactPtr> lst;
    for (const GroupConstPtr &group : qAsConst(groups)) {
        if (group->enabled)
            lst += group->allFiles();
    }
    return lst;
}

FileTags ResolvedProduct::fileTagsForFileName(const QString &fileName) const
{
    FileTags result;
    for (const FileTaggerConstPtr &tagger : qAsConst(fileTaggers)) {
        for (const QRegExp &pattern : tagger->patterns()) {
            if (FileInfo::globMatches(pattern, fileName)) {
                result.unite(tagger->fileTags());
                break;
            }
        }
    }
    return result;
}

void ResolvedProduct::load(PersistentPool &pool)
{
    pool.load(enabled);
    pool.load(fileTags);
    pool.load(name);
    pool.load(profile);
    pool.load(targetName);
    pool.load(sourceDirectory);
    pool.load(destinationDirectory);
    pool.load(missingSourceFiles);
    pool.load(location);
    pool.load(productProperties);
    pool.load(moduleProperties);
    pool.load(rules);
    pool.load(dependencies);
    pool.load(fileTaggers);
    pool.load(modules);
    pool.load(scanners);
    pool.load(groups);
    pool.load(artifactProperties);
    pool.load(probes);
    buildData.reset(pool.load<ProductBuildData *>());
}

void ResolvedProduct::store(PersistentPool &pool) const
{
    pool.store(enabled);
    pool.store(fileTags);
    pool.store(name);
    pool.store(profile);
    pool.store(targetName);
    pool.store(sourceDirectory);
    pool.store(destinationDirectory);
    pool.store(missingSourceFiles);
    pool.store(location);
    pool.store(productProperties);
    pool.store(moduleProperties);
    pool.store(rules);
    pool.store(dependencies);
    pool.store(fileTaggers);
    pool.store(modules);
    pool.store(scanners);
    pool.store(groups);
    pool.store(artifactProperties);
    pool.store(probes);
    pool.store(buildData.data());
}

QList<const ResolvedModule*> topSortModules(const QHash<const ResolvedModule*, QList<const ResolvedModule*> > &moduleChildren,
                                      const QList<const ResolvedModule*> &modules,
                                      Set<QString> &seenModuleNames)
{
    QList<const ResolvedModule*> result;
    for (const ResolvedModule * const m : modules) {
        if (m->name.isNull())
            continue;
        result.append(topSortModules(moduleChildren, moduleChildren.value(m), seenModuleNames));
        if (seenModuleNames.insert(m->name).second)
            result.append(m);
    }
    return result;
}

enum EnvType
{
    BuildEnv, RunEnv
};

static bool findModuleMapRecursively_impl(const QVariantMap &cfg, const QString &moduleName,
        QVariantMap *result)
{
    for (QVariantMap::const_iterator it = cfg.constBegin(); it != cfg.constEnd(); ++it) {
        if (it.key() == moduleName) {
            *result = it.value().toMap();
            return true;
        }
        if (findModuleMapRecursively_impl(it.value().toMap().value(QStringLiteral("modules")).toMap(),
                                          moduleName, result)) {
            return true;
        }
    }
    return false;
}

static QVariantMap findModuleMapRecursively(const QVariantMap &cfg, const QString &moduleName)
{
    QVariantMap result;
    findModuleMapRecursively_impl(cfg, moduleName, &result);
    return result;
}

static QProcessEnvironment getProcessEnvironment(ScriptEngine *engine, EnvType envType,
                                                 const QList<ResolvedModuleConstPtr> &modules,
                                                 const PropertyMapConstPtr &productConfiguration,
                                                 const QProcessEnvironment &env)
{
    QMap<QString, const ResolvedModule *> moduleMap;
    for (const ResolvedModuleConstPtr &module : modules)
        moduleMap.insert(module->name, module.data());

    QHash<const ResolvedModule*, QList<const ResolvedModule*> > moduleParents;
    QHash<const ResolvedModule*, QList<const ResolvedModule*> > moduleChildren;
    for (const ResolvedModuleConstPtr &module : modules) {
        for (const QString &moduleName : qAsConst(module->moduleDependencies)) {
            const ResolvedModule * const depmod = moduleMap.value(moduleName);
            QBS_ASSERT(depmod, return env);
            moduleParents[depmod].append(module.data());
            moduleChildren[module.data()].append(depmod);
        }
    }

    QList<const ResolvedModule *> rootModules;
    for (const ResolvedModuleConstPtr &module : modules) {
        if (moduleParents.value(module.data()).isEmpty()) {
            QBS_ASSERT(module, return env);
            rootModules.append(module.data());
        }
    }

    QProcessEnvironment procenv = env;

    {
        QVariant v;
        v.setValue<void*>(&procenv);
        engine->setProperty("_qbs_procenv", v);
    }

    QScriptValue scope = engine->newObject();
    scope.setPrototype(engine->globalObject());
    TemporaryGlobalObjectSetter tgos(scope);

    Set<QString> seenModuleNames;
    const QList<const ResolvedModule *> &topSortedModules
            = topSortModules(moduleChildren, rootModules, seenModuleNames);
    for (const ResolvedModule * const module : topSortedModules) {
        if ((envType == BuildEnv && module->setupBuildEnvironmentScript->sourceCode.isEmpty()) ||
            (envType == RunEnv && module->setupBuildEnvironmentScript->sourceCode.isEmpty()
             && module->setupRunEnvironmentScript->sourceCode.isEmpty()))
            continue;

        ScriptFunctionConstPtr setupScript;
        if (envType == BuildEnv) {
            setupScript = module->setupBuildEnvironmentScript;
        } else {
            if (!module->setupRunEnvironmentScript)
                setupScript = module->setupBuildEnvironmentScript;
            else
                setupScript = module->setupRunEnvironmentScript;
        }

        // handle imports
        engine->import(setupScript->fileContext, scope);
        JsExtensions::setupExtensions(setupScript->fileContext->jsExtensions(), scope);

        // expose properties of direct module dependencies
        QScriptValue scriptValue;
        QVariantMap productModules = productConfiguration->value()
                .value(QLatin1String("modules")).toMap();
        for (const ResolvedModule * const depmod : moduleChildren.value(module)) {
            scriptValue = engine->newObject();
            QVariantMap moduleCfg = productModules.value(depmod->name).toMap();
            for (QVariantMap::const_iterator it = moduleCfg.constBegin(); it != moduleCfg.constEnd(); ++it)
                scriptValue.setProperty(it.key(), engine->toScriptValue(it.value()));
            scope.setProperty(depmod->name, scriptValue);
        }

        // expose the module's properties
        QVariantMap moduleCfg = findModuleMapRecursively(productModules, module->name);
        for (QVariantMap::const_iterator it = moduleCfg.constBegin(); it != moduleCfg.constEnd(); ++it)
            scope.setProperty(it.key(), engine->toScriptValue(it.value()));

        scriptValue = engine->evaluate(setupScript->sourceCode + QLatin1String("()"));
        if (Q_UNLIKELY(engine->hasErrorOrException(scriptValue))) {
            QString envTypeStr = (envType == BuildEnv
                                  ? QLatin1String("build") : QLatin1String("run"));
            throw ErrorInfo(Tr::tr("Error while setting up %1 environment: %2")
                            .arg(envTypeStr, engine->lastErrorString(scriptValue)),
                            engine->lastErrorLocation(scriptValue, setupScript->location));
        }
    }

    engine->setProperty("_qbs_procenv", QVariant());
    return procenv;
}

void ResolvedProduct::setupBuildEnvironment(ScriptEngine *engine, const QProcessEnvironment &env) const
{
    if (!buildEnvironment.isEmpty())
        return;

    buildEnvironment = getProcessEnvironment(engine, BuildEnv, modules, moduleProperties, env);
}

void ResolvedProduct::setupRunEnvironment(ScriptEngine *engine, const QProcessEnvironment &env) const
{
    if (!runEnvironment.isEmpty())
        return;

    runEnvironment = getProcessEnvironment(engine, RunEnv, modules, moduleProperties, env);
}

void ResolvedProduct::registerArtifactWithChangedInputs(Artifact *artifact)
{
    QBS_CHECK(buildData);
    QBS_CHECK(artifact->product == this);
    QBS_CHECK(artifact->transformer);
    if (artifact->transformer->rule->multiplex) {
        // Reapplication of rules only makes sense for multiplex rules (e.g. linker).
        buildData->artifactsWithChangedInputsPerRule[artifact->transformer->rule] += artifact;
    }
}

void ResolvedProduct::unregisterArtifactWithChangedInputs(Artifact *artifact)
{
    QBS_CHECK(buildData);
    QBS_CHECK(artifact->product == this);
    QBS_CHECK(artifact->transformer);
    buildData->artifactsWithChangedInputsPerRule[artifact->transformer->rule] -= artifact;
}

void ResolvedProduct::unmarkForReapplication(const RuleConstPtr &rule)
{
    QBS_CHECK(buildData);
    buildData->artifactsWithChangedInputsPerRule.remove(rule);
}

bool ResolvedProduct::isMarkedForReapplication(const RuleConstPtr &rule) const
{
    return !buildData->artifactsWithChangedInputsPerRule.value(rule).isEmpty();
}

ArtifactSet ResolvedProduct::lookupArtifactsByFileTag(const FileTag &tag) const
{
    QBS_CHECK(buildData);
    return buildData->artifactsByFileTag.value(tag);
}

ArtifactSet ResolvedProduct::lookupArtifactsByFileTags(const FileTags &tags) const
{
    QBS_CHECK(buildData);
    ArtifactSet set;
    for (const FileTag &tag : tags)
        set = set.unite(buildData->artifactsByFileTag.value(tag));
    return set;
}

ArtifactSet ResolvedProduct::targetArtifacts() const
{
    QBS_CHECK(buildData);
    ArtifactSet taSet;
    for (Artifact * const a : buildData->rootArtifacts()) {
        if (a->fileTags().intersects(fileTags))
            taSet << a;
    }
    return taSet;
}

TopLevelProject *ResolvedProduct::topLevelProject() const
{
    return project->topLevelProject();
}

QString ResolvedProduct::uniqueName(const QString &name, const QString &profile)
{
    QBS_CHECK(!profile.isEmpty());
    return name + QLatin1Char('.') + profile;
}

QString ResolvedProduct::uniqueName() const
{
    return uniqueName(name, profile);
}

static QStringList findGeneratedFiles(const Artifact *base, bool recursive, const FileTags &tags)
{
    QStringList result;
    for (const Artifact *parent : base->parentArtifacts()) {
        if (tags.isEmpty() || parent->fileTags().intersects(tags))
            result << parent->filePath();
        if (recursive)
            result << findGeneratedFiles(parent, true, tags);
    }
    return result;
}

QStringList ResolvedProduct::generatedFiles(const QString &baseFile, bool recursive,
                                            const FileTags &tags) const
{
    ProductBuildData *data = buildData.data();
    if (!data)
        return QStringList();

    for (const Artifact *art : filterByType<Artifact>(data->nodes)) {
        if (art->filePath() == baseFile)
            return findGeneratedFiles(art, recursive, tags);
    }
    return QStringList();
}

QString ResolvedProduct::deriveBuildDirectoryName(const QString &name, const QString &profile)
{
    QString dirName = uniqueName(name, profile);
    const QByteArray hash = QCryptographicHash::hash(dirName.toUtf8(), QCryptographicHash::Sha1);
    return HostOsInfo::rfc1034Identifier(dirName)
            .append(QLatin1Char('.'))
            .append(QString::fromLatin1(hash.toHex().left(8)));
}

QString ResolvedProduct::buildDirectory() const
{
    return productProperties.value(QLatin1String("buildDirectory")).toString();
}

bool ResolvedProduct::isInParentProject(const ResolvedProductConstPtr &other) const
{
    for (const ResolvedProject *otherParent = other->project.data(); otherParent;
         otherParent = otherParent->parentProject.data()) {
        if (otherParent == project.data())
            return true;
    }
    return false;
}

bool ResolvedProduct::builtByDefault() const
{
    return productProperties.value(QLatin1String("builtByDefault"), true).toBool();
}

void ResolvedProduct::cacheExecutablePath(const QString &origFilePath, const QString &fullFilePath)
{
    std::lock_guard<std::mutex> locker(m_executablePathCacheLock);
    m_executablePathCache.insert(origFilePath, fullFilePath);
}

QString ResolvedProduct::cachedExecutablePath(const QString &origFilePath) const
{
    std::lock_guard<std::mutex> locker(m_executablePathCacheLock);
    return m_executablePathCache.value(origFilePath);
}


ResolvedProject::ResolvedProject() : enabled(true), m_topLevelProject(0)
{
}

void ResolvedProject::accept(BuildGraphVisitor *visitor) const
{
    for (const ResolvedProductPtr &product : qAsConst(products))
        product->accept(visitor);
    for (const ResolvedProjectPtr &subProject : qAsConst(subProjects))
        subProject->accept(visitor);
}

TopLevelProject *ResolvedProject::topLevelProject()
{
    if (m_topLevelProject)
        return m_topLevelProject;
    TopLevelProject *tlp = dynamic_cast<TopLevelProject *>(this);
    if (tlp) {
        m_topLevelProject = tlp;
        return m_topLevelProject;
    }
    QBS_CHECK(!parentProject.isNull());
    m_topLevelProject = parentProject->topLevelProject();
    return m_topLevelProject;
}

QList<ResolvedProjectPtr> ResolvedProject::allSubProjects() const
{
    QList<ResolvedProjectPtr> projectList = subProjects;
    for (const ResolvedProjectConstPtr &subProject : qAsConst(subProjects))
        projectList << subProject->allSubProjects();
    return projectList;
}

QList<ResolvedProductPtr> ResolvedProject::allProducts() const
{
    QList<ResolvedProductPtr> productList = products;
    for (const ResolvedProjectConstPtr &subProject : qAsConst(subProjects))
        productList << subProject->allProducts();
    return productList;
}

void ResolvedProject::load(PersistentPool &pool)
{
    pool.load(name);
    pool.load(location);
    pool.load(enabled);
    pool.load(products);
    std::for_each(products.cbegin(), products.cend(),
                  [](const ResolvedProductPtr &p) {
        if (!p->buildData)
            return;
        for (BuildGraphNode * const node : qAsConst(p->buildData->nodes)) {
            node->product = p;

            // restore parent links
            for (BuildGraphNode * const child : qAsConst(node->children))
                child->parents.insert(node);
        }
    });
    pool.load(subProjects);
    pool.load(m_projectProperties);
}

void ResolvedProject::store(PersistentPool &pool) const
{
    pool.store(name);
    pool.store(location);
    pool.store(enabled);
    pool.store(products);
    pool.store(subProjects);
    pool.store(m_projectProperties);
}


TopLevelProject::TopLevelProject()
    : bgLocker(0), locked(false), lastResolveTime(FileTime::oldestTime())
{
}

TopLevelProject::~TopLevelProject()
{
    delete bgLocker;
}

QString TopLevelProject::deriveId(const QVariantMap &config)
{
    const QVariantMap qbsProperties = config.value(QLatin1String("qbs")).toMap();
    const QString configurationName = qbsProperties.value(QLatin1String("configurationName"))
            .toString();
    return configurationName;
}

QString TopLevelProject::deriveBuildDirectory(const QString &buildRoot, const QString &id)
{
    return buildRoot + QLatin1Char('/') + id;
}

void TopLevelProject::setBuildConfiguration(const QVariantMap &config)
{
    m_buildConfiguration = config;
    m_id = deriveId(config);
}

QString TopLevelProject::profile() const
{
    return projectProperties().value(QLatin1String("profile")).toString();
}

QString TopLevelProject::buildGraphFilePath() const
{
    return ProjectBuildData::deriveBuildGraphFilePath(buildDirectory, id());
}

void TopLevelProject::store(Logger logger) const
{
    // TODO: Use progress observer here.

    if (!buildData)
        return;
    if (!buildData->isDirty) {
        logger.qbsDebug() << "[BG] build graph is unchanged in project " << id() << ".";
        return;
    }
    const QString fileName = buildGraphFilePath();
    logger.qbsDebug() << "[BG] storing: " << fileName;
    PersistentPool pool(logger);
    PersistentPool::HeadData headData;
    headData.projectConfig = buildConfiguration();
    pool.setHeadData(headData);
    pool.setupWriteStream(fileName);
    store(pool);
    pool.finalizeWriteStream();
    buildData->isDirty = false;
}

void TopLevelProject::load(PersistentPool &pool)
{
    ResolvedProject::load(pool);
    pool.load(m_id);
    pool.load(usedEnvironment);
    pool.load(canonicalFilePathResults);
    pool.load(fileExistsResults);
    pool.load(directoryEntriesResults);
    pool.load(fileLastModifiedResults);
    pool.load(environment);
    pool.load(probes);
    pool.load(profileConfigs);
    pool.load(overriddenValues);
    pool.load(buildSystemFiles);
    pool.load(lastResolveTime);
    pool.load(warningsEncountered);
    buildData.reset(pool.load<ProjectBuildData *>());
    QBS_CHECK(buildData);
    buildData->isDirty = false;
}

void TopLevelProject::store(PersistentPool &pool) const
{
    ResolvedProject::store(pool);
    pool.store(m_id);
    pool.store(usedEnvironment);
    pool.store(canonicalFilePathResults);
    pool.store(fileExistsResults);
    pool.store(directoryEntriesResults);
    pool.store(fileLastModifiedResults);
    pool.store(environment);
    pool.store(probes);
    pool.store(profileConfigs);
    pool.store(overriddenValues);
    pool.store(buildSystemFiles);
    pool.store(lastResolveTime);
    pool.store(warningsEncountered);
    pool.store(buildData.data());
}

/*!
 * \class SourceWildCards
 * \brief Objects of the \c SourceWildCards class result from giving wildcards in a
 *        \c ResolvedGroup's "files" binding.
 * \sa ResolvedGroup
 */

/*!
  * \variable SourceWildCards::prefix
  * \brief Inherited from the \c ResolvedGroup
  * \sa ResolvedGroup
  */

/*!
 * \variable SourceWildCards::patterns
 * \brief All elements of the \c ResolvedGroup's "files" binding that contain wildcards.
 * \sa ResolvedGroup
 */

/*!
 * \variable SourceWildCards::excludePatterns
 * \brief Corresponds to the \c ResolvedGroup's "excludeFiles" binding.
 * \sa ResolvedGroup
 */

/*!
 * \variable SourceWildCards::files
 * \brief The \c SourceArtifacts resulting from the expanded list of matching files.
 */

Set<QString> SourceWildCards::expandPatterns(const GroupConstPtr &group,
                                              const QString &baseDir, const QString &buildDir)
{
    Set<QString> files = expandPatterns(group, patterns, baseDir, buildDir);
    files -= expandPatterns(group, excludePatterns, baseDir, buildDir);
    return files;
}

Set<QString> SourceWildCards::expandPatterns(const GroupConstPtr &group,
        const QStringList &patterns, const QString &baseDir, const QString &buildDir)
{
    Set<QString> files;
    QString expandedPrefix = prefix;
    if (expandedPrefix.startsWith(QLatin1String("~/")))
        expandedPrefix.replace(0, 1, QDir::homePath());
    for (QString pattern : patterns) {
        pattern.prepend(expandedPrefix);
        pattern.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QStringList parts = pattern.split(QLatin1Char('/'), QString::SkipEmptyParts);
        if (FileInfo::isAbsolute(pattern)) {
            QString rootDir;
            if (HostOsInfo::isWindowsHost() && pattern.at(0) != QLatin1Char('/')) {
                rootDir = parts.takeFirst();
                if (!rootDir.endsWith(QLatin1Char('/')))
                    rootDir.append(QLatin1Char('/'));
            } else {
                rootDir = QLatin1Char('/');
            }
            expandPatterns(files, group, parts, rootDir, buildDir);
        } else {
            expandPatterns(files, group, parts, baseDir, buildDir);
        }
    }

    return files;
}

void SourceWildCards::expandPatterns(Set<QString> &result, const GroupConstPtr &group,
                                     const QStringList &parts,
                                     const QString &baseDir, const QString &buildDir)
{
    // People might build directly in the project source directory. This is okay, since
    // we keep the build data in a "container" directory. However, we must make sure we don't
    // match any generated files therein as source files.
    if (baseDir.startsWith(buildDir))
        return;

    dirTimeStamps.push_back({ baseDir, FileInfo(baseDir).lastModified() });

    QStringList changed_parts = parts;
    bool recursive = false;
    QString part = changed_parts.takeFirst();

    while (part == QLatin1String("**")) {
        recursive = true;

        if (changed_parts.isEmpty()) {
            part = QLatin1String("*");
            break;
        }

        part = changed_parts.takeFirst();
    }

    const bool isDir = !changed_parts.isEmpty();

    const QString &filePattern = part;
    const QDirIterator::IteratorFlags itFlags = recursive
            ? QDirIterator::Subdirectories
            : QDirIterator::NoIteratorFlags;
    QDir::Filters itFilters = isDir
            ? QDir::Dirs
            : QDir::Files | QDir::System
              | QDir::Dirs; // This one is needed to get symbolic links to directories

    if (isDir && !FileInfo::isPattern(filePattern))
        itFilters |= QDir::Hidden;
    if (filePattern != QLatin1String("..") && filePattern != QLatin1String("."))
        itFilters |= QDir::NoDotAndDotDot;

    QDirIterator it(baseDir, QStringList(filePattern), itFilters, itFlags);
    while (it.hasNext()) {
        const QString filePath = it.next();
        if (it.fileInfo().dir().path().startsWith(buildDir))
            continue; // See above.
        if (!isDir && it.fileInfo().isDir() && !it.fileInfo().isSymLink())
            continue;
        if (isDir)
            expandPatterns(result, group, changed_parts, filePath, buildDir);
        else
            result += QDir::cleanPath(filePath);
    }
}

template<typename T> QMap<QString, T> listToMap(const QList<T> &list)
{
    QMap<QString, T> map;
    for (const T &elem : list)
        map.insert(keyFromElem(elem), elem);
    return map;
}

template<typename T> bool listsAreEqual(const QList<T> &l1, const QList<T> &l2)
{
    if (l1.count() != l2.count())
        return false;
    const QMap<QString, T> map1 = listToMap(l1);
    const QMap<QString, T> map2 = listToMap(l2);
    for (const QString &key : map1.keys()) {
        const T value2 = map2.value(key);
        if (!value2)
            return false;
        if (!equals(map1.value(key).data(), value2.data()))
            return false;
    }
    return true;
}

QString keyFromElem(const SourceArtifactPtr &sa) { return sa->absoluteFilePath; }
QString keyFromElem(const RulePtr &r) {
    QString key = r->toString() + r->prepareScript->sourceCode;
    if (r->outputArtifactsScript)
        key += r->outputArtifactsScript->sourceCode;
    for (const auto &a : qAsConst(r->artifacts)) {
        key += a->filePath;
    }
    return key;
}

QString keyFromElem(const ArtifactPropertiesPtr &ap)
{
    QStringList lst = ap->fileTagsFilter().toStringList();
    lst.sort();
    return lst.join(QLatin1Char(','));
}

bool operator==(const SourceArtifactInternal &sa1, const SourceArtifactInternal &sa2)
{
    return sa1.absoluteFilePath == sa2.absoluteFilePath
            && sa1.fileTags == sa2.fileTags
            && sa1.overrideFileTags == sa2.overrideFileTags
            && *sa1.properties == *sa2.properties;
}

bool sourceArtifactSetsAreEqual(const QList<SourceArtifactPtr> &l1,
                                 const QList<SourceArtifactPtr> &l2)
{
    return listsAreEqual(l1, l2);
}

bool operator==(const Rule &r1, const Rule &r2)
{
    if (r1.artifacts.count() != r2.artifacts.count())
        return false;
    for (int i = 0; i < r1.artifacts.count(); ++i) {
        if (!equals(r1.artifacts.at(i).data(), r2.artifacts.at(i).data()))
            return false;
    }

    return r1.module->name == r2.module->name
            && equals(r1.prepareScript.data(), r2.prepareScript.data())
            && equals(r1.outputArtifactsScript.data(), r2.outputArtifactsScript.data())
            && r1.inputs == r2.inputs
            && r1.outputFileTags == r2.outputFileTags
            && r1.auxiliaryInputs == r2.auxiliaryInputs
            && r1.excludedAuxiliaryInputs == r2.excludedAuxiliaryInputs
            && r1.inputsFromDependencies == r2.inputsFromDependencies
            && r1.explicitlyDependsOn == r2.explicitlyDependsOn
            && r1.multiplex == r2.multiplex
            && r1.alwaysRun == r2.alwaysRun;
}

bool ruleListsAreEqual(const QList<RulePtr> &l1, const QList<RulePtr> &l2)
{
    return listsAreEqual(l1, l2);
}

bool operator==(const RuleArtifact &a1, const RuleArtifact &a2)
{
    return a1.filePath == a2.filePath
            && a1.fileTags == a2.fileTags
            && a1.alwaysUpdated == a2.alwaysUpdated
            && Set<RuleArtifact::Binding>::fromStdVector(a1.bindings) ==
               Set<RuleArtifact::Binding>::fromStdVector(a2.bindings);
}

bool operator==(const RuleArtifact::Binding &b1, const RuleArtifact::Binding &b2)
{
    return b1.code == b2.code && b1.name == b2.name;
}

uint qHash(const RuleArtifact::Binding &b)
{
    return qHash(std::make_pair(b.code, b.name.join(QLatin1Char(','))));
}

bool artifactPropertyListsAreEqual(const QList<ArtifactPropertiesPtr> &l1,
                                   const QList<ArtifactPropertiesPtr> &l2)
{
    return listsAreEqual(l1, l2);
}

void ResolvedScanner::load(PersistentPool &pool)
{
    pool.load(module);
    pool.load(inputs);
    pool.load(recursive);
    pool.load(searchPathsScript);
    pool.load(scanScript);
}

void ResolvedScanner::store(PersistentPool &pool) const
{
    pool.store(module);
    pool.store(inputs);
    pool.store(recursive);
    pool.store(searchPathsScript);
    pool.store(scanScript);
}

} // namespace Internal
} // namespace qbs

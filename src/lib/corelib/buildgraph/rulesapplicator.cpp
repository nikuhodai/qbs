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
#include "rulesapplicator.h"

#include "buildgraph.h"
#include "emptydirectoriesremover.h"
#include "productbuilddata.h"
#include "projectbuilddata.h"
#include "qtmocscanner.h"
#include "rulecommands.h"
#include "rulesevaluationcontext.h"
#include "transformer.h"
#include "transformerchangetracking.h"

#include <jsextensions/moduleproperties.h>
#include <language/artifactproperties.h>
#include <language/builtindeclarations.h>
#include <language/language.h>
#include <language/preparescriptobserver.h>
#include <language/propertymapinternal.h>
#include <language/resolvedfilecontext.h>
#include <language/scriptengine.h>
#include <logging/categories.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/scripttools.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stringconstants.h>

#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtScript/qscriptvalueiterator.h>

#include <memory>
#include <vector>

namespace qbs {
namespace Internal {

RulesApplicator::RulesApplicator(
        const ResolvedProductPtr &product,
        const std::unordered_map<QString, const ResolvedProduct *> &productsByName,
        const std::unordered_map<QString, const ResolvedProject *> &projectsByName,
        const Logger &logger)
    : m_product(product)
    , m_productsByName(productsByName)
    , m_projectsByName(projectsByName)
    , m_mocScanner(nullptr)
    , m_logger(logger)
{
}

RulesApplicator::~RulesApplicator()
{
    delete m_mocScanner;
}

void RulesApplicator::applyRule(const RuleConstPtr &rule, const ArtifactSet &inputArtifacts)
{
    if (inputArtifacts.empty() && rule->declaresInputs() && rule->requiresInputs)
        return;

    m_product->topLevelProject()->buildData->setDirty();
    m_createdArtifacts.clear();
    m_invalidatedArtifacts.clear();
    RulesEvaluationContext::Scope s(evalContext().get());

    m_rule = rule;
    m_completeInputSet = inputArtifacts;
    if (rule->name == QLatin1String("QtCoreMocRule")) {
        delete m_mocScanner;
        m_mocScanner = new QtMocScanner(m_product, scope());
    }
    QScriptValue prepareScriptContext = engine()->newObject();
    prepareScriptContext.setPrototype(engine()->globalObject());
    setupScriptEngineForFile(engine(), m_rule->prepareScript.fileContext(), scope(),
                             ObserveMode::Enabled);
    setupScriptEngineForProduct(engine(), m_product.get(), m_rule->module.get(),
                                prepareScriptContext, true);

    if (m_rule->multiplex) { // apply the rule once for a set of inputs
        doApply(inputArtifacts, prepareScriptContext);
    } else { // apply the rule once for each input
        for (Artifact * const inputArtifact : inputArtifacts) {
            ArtifactSet lst;
            lst += inputArtifact;
            doApply(lst, prepareScriptContext);
        }
    }
}

void RulesApplicator::handleRemovedRuleOutputs(const ArtifactSet &inputArtifacts,
        const ArtifactSet &outputArtifactsToRemove, const Logger &logger)
{
    ArtifactSet artifactsToRemove;
    const TopLevelProject *project = nullptr;
    for (Artifact * const removedArtifact : outputArtifactsToRemove) {
        qCDebug(lcBuildGraph) << "dynamic rule removed output artifact"
                              << removedArtifact->toString();
        if (!project)
            project = removedArtifact->product->topLevelProject();
        project->buildData->removeArtifactAndExclusiveDependents(removedArtifact, logger, true,
                                                                 &artifactsToRemove);
    }
    EmptyDirectoriesRemover(project, logger).removeEmptyParentDirectories(artifactsToRemove);
    for (Artifact * const artifact : qAsConst(artifactsToRemove)) {
        QBS_CHECK(!inputArtifacts.contains(artifact));
        delete artifact;
    }
}

ArtifactSet RulesApplicator::collectAuxiliaryInputs(const Rule *rule,
                                                    const ResolvedProduct *product)
{
    return collectAdditionalInputs(rule->auxiliaryInputs, rule, product,
                                   CurrentProduct | Dependencies);
}

static void copyProperty(const QString &name, const QScriptValue &src, QScriptValue dst)
{
    dst.setProperty(name, src.property(name));
}

static QStringList toStringList(const ArtifactSet &artifacts)
{
    QStringList lst;
    for (const Artifact * const artifact : artifacts) {
        const QString str = artifact->filePath() + QLatin1String(" [")
                + artifact->fileTags().toStringList().join(QLatin1String(", ")) + QLatin1Char(']');
        lst << str;
    }
    return lst;
}

void RulesApplicator::doApply(const ArtifactSet &inputArtifacts, QScriptValue &prepareScriptContext)
{
    evalContext()->checkForCancelation();

    qCDebug(lcBuildGraph) << "apply rule" << m_rule->toString()
                          << toStringList(inputArtifacts).join(QLatin1String(",\n            "));

    QList<std::pair<const RuleArtifact *, Artifact *> > ruleArtifactArtifactMap;
    QList<Artifact *> outputArtifacts;

    m_transformer = Transformer::create();
    m_transformer->rule = m_rule;
    m_transformer->inputs = inputArtifacts;
    m_transformer->explicitlyDependsOn = collectExplicitlyDependsOn();
    m_transformer->alwaysRun = m_rule->alwaysRun;
    m_oldTransformer.reset();

    engine()->clearRequestedProperties();

    // create the output artifacts from the set of input artifacts
    m_transformer->setupInputs(prepareScriptContext);
    m_transformer->setupExplicitlyDependsOn(prepareScriptContext);
    copyProperty(StringConstants::inputsVar(), prepareScriptContext, scope());
    copyProperty(StringConstants::inputVar(), prepareScriptContext, scope());
    copyProperty(StringConstants::explicitlyDependsOnVar(), prepareScriptContext, scope());
    copyProperty(StringConstants::productVar(), prepareScriptContext, scope());
    copyProperty(StringConstants::projectVar(), prepareScriptContext, scope());
    if (m_rule->isDynamic()) {
        outputArtifacts = runOutputArtifactsScript(inputArtifacts,
                    ScriptEngine::argumentList(Rule::argumentNamesForOutputArtifacts(), scope()));
    } else {
        Set<QString> outputFilePaths;
        for (const RuleArtifactConstPtr &ruleArtifact : m_rule->artifacts) {
            Artifact * const outputArtifact
                    = createOutputArtifactFromRuleArtifact(ruleArtifact, inputArtifacts,
                                                           &outputFilePaths);
            if (!outputArtifact)
                continue;
            outputArtifacts.push_back(outputArtifact);
            ruleArtifactArtifactMap.push_back({ ruleArtifact.get(), outputArtifact });
        }
        if (m_rule->artifacts.empty()) {
            outputArtifacts.push_back(createOutputArtifactFromRuleArtifact(nullptr, inputArtifacts,
                                                                           &outputFilePaths));
        }
    }

    ArtifactSet newOutputs = ArtifactSet::fromList(outputArtifacts);
    const ArtifactSet oldOutputs = collectOldOutputArtifacts(inputArtifacts);
    handleRemovedRuleOutputs(m_completeInputSet, oldOutputs - newOutputs, m_logger);

    if (outputArtifacts.empty())
        return;

    for (Artifact * const outputArtifact : qAsConst(outputArtifacts)) {
        for (Artifact * const dependency : qAsConst(m_transformer->explicitlyDependsOn))
            connect(outputArtifact, dependency);
    }

    if (inputArtifacts != m_transformer->inputs)
        m_transformer->setupInputs(prepareScriptContext);

    // change the transformer outputs according to the bindings in Artifact
    QScriptValue scriptValue;
    if (!ruleArtifactArtifactMap.empty())
        engine()->setGlobalObject(prepareScriptContext);
    for (auto it = ruleArtifactArtifactMap.crbegin(), end = ruleArtifactArtifactMap.crend();
         it != end; ++it) {
        const RuleArtifact *ra = it->first;
        if (ra->bindings.empty())
            continue;

        // expose attributes of this artifact
        Artifact *outputArtifact = it->second;
        outputArtifact->properties = outputArtifact->properties->clone();

        scope().setProperty(StringConstants::fileNameProperty(),
                            engine()->toScriptValue(outputArtifact->filePath()));
        scope().setProperty(StringConstants::fileTagsProperty(),
                            toScriptValue(engine(), outputArtifact->fileTags().toStringList()));

        QVariantMap artifactModulesCfg = outputArtifact->properties->value();
        for (const auto &binding : ra->bindings) {
            scriptValue = engine()->evaluate(binding.code);
            if (Q_UNLIKELY(engine()->hasErrorOrException(scriptValue))) {
                QString msg = QLatin1String("evaluating rule binding '%1': %2");
                throw ErrorInfo(msg.arg(binding.name.join(QLatin1Char('.')),
                                        engine()->lastErrorString(scriptValue)),
                                engine()->lastErrorLocation(scriptValue, binding.location));
            }
            setConfigProperty(artifactModulesCfg, binding.name, scriptValue.toVariant());
        }
        outputArtifact->properties->setValue(artifactModulesCfg);
    }
    if (!ruleArtifactArtifactMap.empty())
        engine()->setGlobalObject(prepareScriptContext.prototype());

    m_transformer->setupOutputs(prepareScriptContext);
    m_transformer->createCommands(engine(), m_rule->prepareScript,
            ScriptEngine::argumentList(Rule::argumentNamesForPrepare(), prepareScriptContext));
    if (Q_UNLIKELY(m_transformer->commands.empty()))
        throw ErrorInfo(Tr::tr("There is a rule without commands: %1.")
                        .arg(m_rule->toString()), m_rule->prepareScript.location());
    if (!m_oldTransformer || m_oldTransformer->outputs != m_transformer->outputs
            || m_oldTransformer->inputs != m_transformer->inputs
            || m_oldTransformer->explicitlyDependsOn != m_transformer->explicitlyDependsOn
            || m_oldTransformer->commands != m_transformer->commands
            || commandsNeedRerun(m_transformer.get(), m_product.get(), m_productsByName,
                                 m_projectsByName)) {
        for (Artifact * const output : outputArtifacts) {
            output->clearTimestamp();
            m_invalidatedArtifacts += output;
        }
    }
    m_transformer->commandsNeedChangeTracking = false;
}

ArtifactSet RulesApplicator::collectOldOutputArtifacts(const ArtifactSet &inputArtifacts) const
{
    ArtifactSet result;
    for (Artifact * const a : inputArtifacts) {
        for (Artifact *p : a->parentArtifacts()) {
            QBS_CHECK(p->transformer);
            if (p->transformer->rule == m_rule && p->transformer->inputs.contains(a))
                result += p;
        }
    }
    return result;
}

ArtifactSet RulesApplicator::collectAdditionalInputs(const FileTags &tags, const Rule *rule,
                                                     const ResolvedProduct *product,
                                                     InputsSources inputsSources)
{
    ArtifactSet artifacts;
    for (const FileTag &fileTag : tags) {
        for (Artifact *dependency : product->lookupArtifactsByFileTag(fileTag)) {
            // Skip excluded inputs.
            if (dependency->fileTags().intersects(rule->excludedInputs))
                continue;

            // Two cases are considered:
            // 1) An artifact is considered a dependency when it's part of the current product.
            // 2) An artifact marked with filesAreTargets: true inside a Group inside of a
            // Module also ends up in the results returned by product->lookupArtifactsByFileTag,
            // so it should be considered conceptually as a "dependent product artifact".
            if ((inputsSources.testFlag(CurrentProduct) && !dependency->isTargetOfModule())
                 || (inputsSources.testFlag(Dependencies) && dependency->isTargetOfModule())) {
                artifacts << dependency;
            }
        }

        if (inputsSources.testFlag(Dependencies)) {
            for (const ResolvedProductConstPtr &depProduct : product->dependencies) {
                for (Artifact * const ta : depProduct->targetArtifacts()) {
                    if (ta->fileTags().contains(fileTag)
                            && !ta->fileTags().intersects(rule->excludedInputs)) {
                        artifacts << ta;
                    }
                }
            }
        }
    }
    return artifacts;
}

ArtifactSet RulesApplicator::collectExplicitlyDependsOn()
{
   ArtifactSet first = collectAdditionalInputs(
               m_rule->explicitlyDependsOn, m_rule.get(), m_product.get(), CurrentProduct);
   ArtifactSet second = collectAdditionalInputs(
               m_rule->explicitlyDependsOnFromDependencies, m_rule.get(), m_product.get(),
               Dependencies);
   return first.unite(second);
}

Artifact *RulesApplicator::createOutputArtifactFromRuleArtifact(
        const RuleArtifactConstPtr &ruleArtifact, const ArtifactSet &inputArtifacts,
        Set<QString> *outputFilePaths)
{
    QString outputPath;
    FileTags fileTags;
    bool alwaysUpdated;
    if (ruleArtifact) {
        QScriptValue scriptValue = engine()->evaluate(ruleArtifact->filePath,
                                                      ruleArtifact->filePathLocation.filePath(),
                                                      ruleArtifact->filePathLocation.line());
        if (Q_UNLIKELY(engine()->hasErrorOrException(scriptValue)))
            throw engine()->lastError(scriptValue, ruleArtifact->filePathLocation);
        outputPath = scriptValue.toString();
        fileTags = ruleArtifact->fileTags;
        alwaysUpdated = ruleArtifact->alwaysUpdated;
    } else {
        outputPath = QStringLiteral("__dummyoutput__");
        QByteArray hashInput = m_rule->toString().toLatin1();
        for (const Artifact * const input : inputArtifacts)
            hashInput += input->filePath().toLatin1();
        outputPath += QLatin1String(QCryptographicHash::hash(hashInput, QCryptographicHash::Sha1)
                                    .toHex().left(16));
        fileTags = m_rule->outputFileTags;
        alwaysUpdated = false;
    }
    outputPath = FileInfo::resolvePath(m_product->buildDirectory(), outputPath);
    if (Q_UNLIKELY(!outputFilePaths->insert(outputPath).second)) {
        throw ErrorInfo(Tr::tr("Rule %1 already created '%2'.")
                        .arg(m_rule->toString(), outputPath));
    }
    return createOutputArtifact(outputPath, fileTags, alwaysUpdated, inputArtifacts);
}

Artifact *RulesApplicator::createOutputArtifact(const QString &filePath, const FileTags &fileTags,
        bool alwaysUpdated, const ArtifactSet &inputArtifacts)
{
    QString outputPath = filePath;
    // don't let the output artifact "escape" its build dir
    outputPath.replace(StringConstants::dotDot(), QStringLiteral("dotdot"));
    outputPath = resolveOutPath(outputPath);

    Artifact *outputArtifact = lookupArtifact(m_product, outputPath);
    if (outputArtifact) {
        const Transformer * const transformer = outputArtifact->transformer.get();
        if (transformer && transformer->rule != m_rule) {
            QString e = Tr::tr("Conflicting rules for producing %1 %2 \n")
                    .arg(outputArtifact->filePath(),
                         QLatin1Char('[') +
                         outputArtifact->fileTags().toStringList().join(QLatin1String(", "))
                         + QLatin1Char(']'));
            QString str = QLatin1Char('[') + m_rule->inputs.toStringList().join(QLatin1String(", "))
               + QLatin1String("] -> [") + outputArtifact->fileTags().toStringList()
                    .join(QLatin1String(", ")) + QLatin1Char(']');

            e += QString::fromLatin1("  while trying to apply:   %1:%2:%3  %4\n")
                .arg(m_rule->prepareScript.location().filePath())
                .arg(m_rule->prepareScript.location().line())
                .arg(m_rule->prepareScript.location().column())
                .arg(str);

            e += QString::fromLatin1("  was already defined in:  %1:%2:%3  %4\n")
                .arg(transformer->rule->prepareScript.location().filePath())
                .arg(transformer->rule->prepareScript.location().line())
                .arg(transformer->rule->prepareScript.location().column())
                .arg(str);

            throw ErrorInfo(e);
        }
        if (transformer && !m_rule->multiplex && transformer->inputs != inputArtifacts) {
            QBS_CHECK(inputArtifacts.size() == 1);
            QBS_CHECK(transformer->inputs.size() == 1);
            ErrorInfo error(Tr::tr("Conflicting instances of rule '%1':").arg(m_rule->toString()),
                            m_rule->prepareScript.location());
            error.append(Tr::tr("Output artifact '%1' is to be produced from input "
                                "artifacts '%2' and '%3', but the rule is not a multiplex rule.")
                         .arg(outputArtifact->filePath(),
                              (*transformer->inputs.cbegin())->filePath(),
                              (*inputArtifacts.cbegin())->filePath()));
            throw error;
        }
        m_transformer->rescueChangeTrackingData(outputArtifact->transformer);
        m_oldTransformer = outputArtifact->transformer;
    } else {
        std::unique_ptr<Artifact> newArtifact(new Artifact);
        newArtifact->artifactType = Artifact::Generated;
        newArtifact->setFilePath(outputPath);
        insertArtifact(m_product, newArtifact.get());
        m_createdArtifacts += newArtifact.get();
        outputArtifact = newArtifact.release();
    }

    outputArtifact->alwaysUpdated = alwaysUpdated;
    outputArtifact->properties = m_product->moduleProperties;

    FileTags outputArtifactFileTags = fileTags.empty()
            ? m_product->fileTagsForFileName(outputArtifact->fileName()) : fileTags;
    for (const ArtifactPropertiesConstPtr &props : m_product->artifactProperties) {
        if (outputArtifactFileTags.intersects(props->fileTagsFilter())) {
            outputArtifact->properties = props->propertyMap();
            outputArtifactFileTags += props->extraFileTags();
            break;
        }
    }
    outputArtifact->setFileTags(outputArtifactFileTags);

    // Let a positive value of qbs.install imply the file tag "installable".
    if (outputArtifact->properties->qbsPropertyValue(StringConstants::installProperty()).toBool())
        outputArtifact->addFileTag("installable");

    for (Artifact * const inputArtifact : inputArtifacts) {
        QBS_CHECK(outputArtifact != inputArtifact);
        connect(outputArtifact, inputArtifact);
    }

    outputArtifact->transformer = m_transformer;
    m_transformer->outputs.insert(outputArtifact);
    QBS_CHECK(m_rule->multiplex || m_transformer->inputs.size() == 1);

    return outputArtifact;
}

class RuleOutputArtifactsException : public ErrorInfo
{
public:
    using ErrorInfo::ErrorInfo;
};

QList<Artifact *> RulesApplicator::runOutputArtifactsScript(const ArtifactSet &inputArtifacts,
        const QScriptValueList &args)
{
    QList<Artifact *> lst;
    QScriptValue fun = engine()->evaluate(m_rule->outputArtifactsScript.sourceCode(),
                                          m_rule->outputArtifactsScript.location().filePath(),
                                          m_rule->outputArtifactsScript.location().line());
    if (!fun.isFunction())
        throw ErrorInfo(QLatin1String("Function expected."),
                        m_rule->outputArtifactsScript.location());
    QScriptValue res = fun.call(QScriptValue(), args);
    engine()->releaseResourcesOfScriptObjects();
    if (engine()->hasErrorOrException(res))
        throw engine()->lastError(res, m_rule->outputArtifactsScript.location());
    if (!res.isArray())
        throw ErrorInfo(Tr::tr("Rule.outputArtifacts must return an array of objects."),
                        m_rule->outputArtifactsScript.location());
    const quint32 c = res.property(StringConstants::lengthProperty()).toUInt32();
    for (quint32 i = 0; i < c; ++i) {
        try {
            lst.push_back(createOutputArtifactFromScriptValue(res.property(i), inputArtifacts));
        } catch (const RuleOutputArtifactsException &roae) {
            ErrorInfo ei = roae;
            ei.prepend(Tr::tr("Error in Rule.outputArtifacts[%1]").arg(i),
                       m_rule->outputArtifactsScript.location());
            throw ei;
        }
    }
    return lst;
}

class ArtifactBindingsExtractor
{
    struct Entry
    {
        Entry(const QString &module, const QString &name, const QVariant &value)
            : module(module), name(name), value(value)
        {}

        QString module;
        QString name;
        QVariant value;
    };
    std::vector<Entry> m_propertyValues;

    static Set<QString> getArtifactItemPropertyNames()
    {
        Set<QString> s;
        for (const PropertyDeclaration &pd :
                 BuiltinDeclarations::instance().declarationsForType(
                     ItemType::Artifact).properties()) {
            s.insert(pd.name());
        }
        s.insert(StringConstants::explicitlyDependsOnProperty());
        return s;
    }

    void extractPropertyValues(const QScriptValue &obj, const QString &moduleName = QString())
    {
        QScriptValueIterator svit(obj);
        while (svit.hasNext()) {
            svit.next();
            const QString name = svit.name();
            if (moduleName.isEmpty()) {
                // Ignore property names that are part of the Artifact item.
                static const Set<QString> artifactItemPropertyNames
                        = getArtifactItemPropertyNames();
                if (artifactItemPropertyNames.contains(name))
                    continue;
            }

            const QScriptValue value = svit.value();
            if (value.isObject() && !value.isArray() && !value.isError() && !value.isRegExp()) {
                QString newModuleName;
                if (!moduleName.isEmpty())
                    newModuleName.append(moduleName + QLatin1Char('.'));
                newModuleName.append(name);
                extractPropertyValues(value, newModuleName);
            } else {
                m_propertyValues.emplace_back(moduleName, name, value.toVariant());
            }
        }
    }
public:
    void apply(Artifact *outputArtifact, const QScriptValue &obj)
    {
        extractPropertyValues(obj);
        if (m_propertyValues.empty())
            return;

        outputArtifact->properties = outputArtifact->properties->clone();
        QVariantMap artifactCfg = outputArtifact->properties->value();
        for (const auto &e : m_propertyValues)
            setConfigProperty(artifactCfg, {e.module, e.name}, e.value);
        outputArtifact->properties->setValue(artifactCfg);
    }
};

Artifact *RulesApplicator::createOutputArtifactFromScriptValue(const QScriptValue &obj,
        const ArtifactSet &inputArtifacts)
{
    if (!obj.isObject()) {
        throw ErrorInfo(Tr::tr("Elements of the Rule.outputArtifacts array must be "
                               "of Object type."), m_rule->outputArtifactsScript.location());
    }
    const QString unresolvedFilePath
            = obj.property(StringConstants::filePathProperty()).toVariant().toString();
    if (unresolvedFilePath.isEmpty()) {
        throw RuleOutputArtifactsException(
                Tr::tr("Property filePath must be a non-empty string."));
    }
    const QString filePath = FileInfo::resolvePath(m_product->buildDirectory(), unresolvedFilePath);
    const FileTags fileTags = FileTags::fromStringList(
                obj.property(StringConstants::fileTagsProperty()).toVariant().toStringList());
    const QVariant alwaysUpdatedVar
            = obj.property(StringConstants::alwaysUpdatedProperty()).toVariant();
    const bool alwaysUpdated = alwaysUpdatedVar.isValid() ? alwaysUpdatedVar.toBool() : true;
    Artifact *output = createOutputArtifact(filePath, fileTags, alwaysUpdated, inputArtifacts);
    if (output->fileTags().empty()) {
        // Check the file tags after file taggers were run.
        throw RuleOutputArtifactsException(
                Tr::tr("Property fileTags for artifact '%1' must be a non-empty string list. "
                       "Alternatively, a FileTagger can be provided.")
                    .arg(unresolvedFilePath));
    }
    const FileTags explicitlyDependsOn = FileTags::fromStringList(
                obj.property(StringConstants::explicitlyDependsOnProperty())
                .toVariant().toStringList());
    for (const FileTag &tag : explicitlyDependsOn) {
        for (Artifact * const dependency : m_product->lookupArtifactsByFileTag(tag))
            connect(output, dependency);
    }
    ArtifactBindingsExtractor().apply(output, obj);
    return output;
}

QString RulesApplicator::resolveOutPath(const QString &path) const
{
    QString buildDir = m_product->topLevelProject()->buildDirectory;
    QString result = FileInfo::resolvePath(buildDir, path);
    result = QDir::cleanPath(result);
    return result;
}

const RulesEvaluationContextPtr &RulesApplicator::evalContext() const
{
    return m_product->topLevelProject()->buildData->evaluationContext;
}

ScriptEngine *RulesApplicator::engine() const
{
    return evalContext()->engine();
}

QScriptValue RulesApplicator::scope() const
{
    return evalContext()->scope();
}

} // namespace Internal
} // namespace qbs

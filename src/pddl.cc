/**
 * pddl.cc
 *
 * Copyright 2018. All Rights Reserved.
 *
 * Created: November 28, 2018
 * Authors: Toki Migimatsu
 */

#include "symbolic/pddl.h"

#include <VAL/FlexLexer.h>
#include <VAL/typecheck.h>

#include <fstream>  // std::ifstream
#include <string>   // std::string
#include <utility>  // std::move

#include "symbolic/utils/parameter_generator.h"

extern int yyparse();
extern int yydebug;

namespace VAL {

// Expected in pddl+.cpp
parse_category* top_thing = nullptr;
analysis* current_analysis = nullptr;
yyFlexLexer* yfl = nullptr;

// Expected in typechecker.cpp
bool Verbose = false;
std::ostream* report = nullptr;

}  // namespace VAL

const char* current_filename = nullptr;  // Expected in parse_error.h

namespace {

std::unique_ptr<VAL::analysis> ParsePddl(const std::string& filename_domain,
                                         const std::string& filename_problem) {
  std::unique_ptr<VAL::analysis> analysis = std::make_unique<VAL::analysis>();
  yyFlexLexer yfl;

  VAL::current_analysis = analysis.get();
  VAL::yfl = &yfl;
  yydebug = 0;  // Set to 1 to output yacc trace

  // Parse domain
  current_filename = filename_domain.c_str();
  std::ifstream pddl_domain(filename_domain);
  yfl.switch_streams(&pddl_domain, &std::cout);
  yyparse();
  if (analysis->the_domain == nullptr) {
    throw std::runtime_error("ParsePddl(): Unable to parse domain from file: " +
                             std::string(filename_domain));
  }

  // Parse problem
  current_filename = filename_problem.c_str();
  std::ifstream pddl_problem(filename_problem);
  yfl.switch_streams(&pddl_problem, &std::cout);
  yyparse();
  if (analysis->the_domain == nullptr) {
    throw std::runtime_error(
        "ParsePddl(): Unable to parse problem from file: " +
        std::string(filename_problem));
  }

  return analysis;
}

using ::symbolic::Action;
using ::symbolic::Axiom;
using ::symbolic::DerivedPredicate;
using ::symbolic::Object;
using ::symbolic::Pddl;
using ::symbolic::Proposition;
using ::symbolic::State;

State ParseState(const Pddl& pddl, const std::set<std::string>& str_state) {
  State state;
  for (const std::string& str_prop : str_state) {
    state.emplace(pddl, str_prop);
  }
  return state;
}

std::vector<Object> GetObjects(const VAL::domain& domain,
                               const VAL::problem& problem) {
  std::vector<Object> objects =
      symbolic::Object::CreateList(domain.types, domain.constants);
  const std::vector<Object> objects_2 =
      symbolic::Object::CreateList(domain.types, problem.objects);
  objects.insert(objects.end(), objects_2.begin(), objects_2.end());
  return objects;
}

std::unordered_map<std::string, std::vector<Object>> CreateObjectTypeMap(
    const std::vector<Object>& objects) {
  std::unordered_map<std::string, std::vector<Object>> object_map;
  for (const Object& object : objects) {
    std::vector<std::string> types = object.type().ListTypes();
    for (const std::string& type : types) {
      object_map[type].push_back(object);
    }
  }
  return object_map;
}

std::vector<Action> GetActions(const Pddl& pddl, const VAL::domain& domain) {
  std::vector<Action> actions;
  for (const VAL::operator_* op : *domain.ops) {
    const auto* a = dynamic_cast<const VAL::action*>(op);
    if (a == nullptr) continue;
    actions.emplace_back(pddl, op);
  }
  return actions;
}

std::vector<Axiom> GetAxioms(const Pddl& pddl, const VAL::domain& domain) {
  std::vector<Axiom> axioms;
  for (const VAL::operator_* op : *domain.ops) {
    const auto* a = dynamic_cast<const VAL::axiom*>(op);
    if (a == nullptr) continue;
    axioms.emplace_back(pddl, op);
  }
  return axioms;
}

std::vector<DerivedPredicate> GetDerivedPredicates(const Pddl& pddl,
                                                   const VAL::domain& domain) {
  std::vector<DerivedPredicate> predicates;
  predicates.reserve(domain.drvs->size());
  for (const VAL::derivation_rule* drv : *domain.drvs) {
    predicates.emplace_back(pddl, drv);
  }
  return predicates;
}

State GetInitialState(const VAL::domain& domain, const VAL::problem& problem) {
  State initial_state;
  for (const VAL::simple_effect* effect : problem.initial_state->add_effects) {
    std::vector<Object> arguments;
    arguments.reserve(effect->prop->args->size());
    for (const VAL::parameter_symbol* arg : *effect->prop->args) {
      arguments.emplace_back(domain.types, arg);
    }
    initial_state.emplace(effect->prop->head->getName(), std::move(arguments));
  }
  // for (const Object& object : objects) {
  //   initial_state.emplace("=", std::vector<Object>{object, object});
  // }
  return initial_state;
}

State Apply(const State& state, const Action& action,
            const std::vector<Object>& arguments,
            const std::vector<DerivedPredicate>& predicates) {
  State next_state = action.Apply(state, arguments);
  DerivedPredicate::Apply(predicates, &next_state);
  return next_state;
}

bool Apply(const Action& action, const std::vector<Object>& arguments,
           const std::vector<DerivedPredicate>& predicates, State* state) {
  bool is_changed = action.Apply(arguments, state);
  is_changed |= DerivedPredicate::Apply(predicates, state);
  return is_changed;
}

}  // namespace

namespace symbolic {

Pddl::Pddl(const std::string& domain_pddl, const std::string& problem_pddl)
    : analysis_(ParsePddl(domain_pddl, problem_pddl)),
      domain_(*analysis_->the_domain),
      problem_(*analysis_->the_problem),
      objects_(GetObjects(domain_, problem_)),
      object_map_(CreateObjectTypeMap(objects_)),
      actions_(GetActions(*this, domain_)),
      axioms_(GetAxioms(*this, domain_)),
      derived_predicates_(GetDerivedPredicates(*this, domain_)),
      initial_state_(GetInitialState(domain_, problem_)),
      goal_(*this, problem_.the_goal) {}

bool Pddl::IsValid(bool verbose, std::ostream& os) const {
  VAL::Verbose = verbose;
  VAL::report = &os;

  VAL::TypeChecker tc(analysis_.get());
  const bool is_domain_valid = tc.typecheckDomain();
  const bool is_problem_valid = tc.typecheckProblem();

  // Output the errors from all input files
  if (verbose) {
    analysis_->error_list.report();
  }

  return is_domain_valid && is_problem_valid;
}

State Pddl::NextState(const State& state,
                      const std::string& action_call) const {
  // Parse strings
  const std::pair<Action, std::vector<Object>> action_args =
      Action::Parse(*this, action_call);
  const Action& action = action_args.first;
  const std::vector<Object>& arguments = action_args.second;

  return Apply(state, action, arguments, derived_predicates());
}
std::set<std::string> Pddl::NextState(const std::set<std::string>& str_state,
                                      const std::string& action_call) const {
  // Parse strings
  State state = ParseState(*this, str_state);
  const std::pair<Action, std::vector<Object>> action_args =
      Action::Parse(*this, action_call);
  const Action& action = action_args.first;
  const std::vector<Object>& arguments = action_args.second;

  Apply(action, arguments, derived_predicates(), &state);
  return Stringify(state);
}

bool Pddl::IsValidAction(const State& state,
                         const std::string& action_call) const {
  // Parse strings
  const std::pair<Action, std::vector<Object>> action_args =
      Action::Parse(*this, action_call);
  const Action& action = action_args.first;
  const std::vector<Object>& arguments = action_args.second;

  return action.IsValid(state, arguments);
}
bool Pddl::IsValidAction(const std::set<std::string>& str_state,
                         const std::string& action_call) const {
  return IsValidAction(ParseState(*this, str_state), action_call);
}

bool Pddl::IsValidTuple(const State& state, const std::string& action_call,
                        const State& next_state) const {
  // Parse strings
  const std::pair<Action, std::vector<Object>> action_args =
      Action::Parse(*this, action_call);
  const Action& action = action_args.first;
  const std::vector<Object>& arguments = action_args.second;

  return action.IsValid(state, arguments) &&
         Apply(state, action, arguments, derived_predicates()) == next_state;
}
bool Pddl::IsValidTuple(const std::set<std::string>& str_state,
                        const std::string& action_call,
                        const std::set<std::string>& str_next_state) const {
  // Parse strings
  const State state = ParseState(*this, str_state);
  const State next_state = ParseState(*this, str_next_state);
  return IsValidTuple(state, action_call, next_state);
}

bool Pddl::IsGoalSatisfied(const std::set<std::string>& str_state) const {
  // Parse strings
  const State state = ParseState(*this, str_state);

  return goal_(state);
}

bool Pddl::IsValidPlan(const std::vector<std::string>& action_skeleton) const {
  State state = initial_state_;
  for (const std::string& action_call : action_skeleton) {
    // Parse strings
    std::pair<Action, std::vector<Object>> action_args =
        Action::Parse(*this, action_call);
    const Action& action = action_args.first;
    const std::vector<Object>& arguments = action_args.second;

    if (!action.IsValid(state, arguments)) return false;
    Apply(action, arguments, derived_predicates(), &state);
  }
  return goal_(state);
}

std::vector<std::vector<Object>> Pddl::ListValidArguments(
    const State& state, const Action& action) const {
  std::vector<std::vector<Object>> arguments;
  ParameterGenerator param_gen(object_map(), action.parameters());
  for (const std::vector<Object>& args : param_gen) {
    if (action.IsValid(state, args)) arguments.push_back(args);
  }
  return arguments;
}
std::vector<std::vector<std::string>> Pddl::ListValidArguments(
    const std::set<std::string>& str_state,
    const std::string& action_name) const {
  // Parse strings
  const State state = ParseState(*this, str_state);
  const Action action(*this, action_name);
  const std::vector<std::vector<Object>> arguments =
      ListValidArguments(state, action);
  return Stringify(arguments);
}

// std::set<std::string> Pddl::initial_state_str() const {
//   return StringifyState(initial_state_);
// }

// std::vector<std::string> Pddl::actions_str() const {
//   return StringifyActions(actions_);
// }

std::vector<std::string> Pddl::ListValidActions(const State& state) const {
  std::vector<std::string> actions;
  for (const Action& action : actions_) {
    const std::vector<std::vector<Object>> arguments =
        ListValidArguments(state, action);
    for (const std::vector<Object>& args : arguments) {
      actions.emplace_back(action.to_string(args));
    }
  }
  return actions;
}

std::vector<std::string> Pddl::ListValidActions(
    const std::set<std::string>& state) const {
  return ListValidActions(ParseState(*this, state));
}

std::set<std::string> Stringify(const State& state) {
  std::set<std::string> str_state;
  for (const Proposition& prop : state) {
    str_state.emplace(prop.to_string());
  }
  return str_state;
}

std::vector<std::string> Stringify(const std::vector<Action>& actions) {
  std::vector<std::string> str_actions;
  str_actions.reserve(actions.size());
  for (const Action& action : actions) {
    str_actions.emplace_back(action.name());
  }
  return str_actions;
}

std::vector<std::vector<std::string>> Stringify(
    const std::vector<std::vector<Object>>& arguments) {
  std::vector<std::vector<std::string>> str_arguments;
  str_arguments.reserve(arguments.size());
  for (const std::vector<Object>& args : arguments) {
    std::vector<std::string> str_args;
    str_args.reserve(args.size());
    for (const Object& arg : args) {
      str_args.emplace_back(arg.name());
    }
    str_arguments.emplace_back(std::move(str_args));
  }
  return str_arguments;
}

std::vector<std::string> Stringify(const std::vector<Object>& objects) {
  std::vector<std::string> str_objects;
  str_objects.reserve(objects.size());
  for (const Object& object : objects) {
    str_objects.emplace_back(object.name());
  }
  return str_objects;
}

std::ostream& operator<<(std::ostream& os, const Pddl& pddl) {
  os << pddl.domain() << std::endl;
  os << pddl.problem() << std::endl;
  return os;
}

}  // namespace symbolic

namespace {

void PrintGoal(std::ostream& os, const VAL::goal* goal, size_t depth) {
  std::string padding(depth, '\t');

  // Proposition
  const auto* simple_goal = dynamic_cast<const VAL::simple_goal*>(goal);
  if (simple_goal != nullptr) {
    const VAL::proposition* prop = simple_goal->getProp();
    os << padding << prop->head->getName() << *prop->args << " [" << prop << "]"
       << std::endl;
    return;
  }

  // Conjunction
  const auto* conj_goal = dynamic_cast<const VAL::conj_goal*>(goal);
  if (conj_goal != nullptr) {
    os << padding << "and:" << std::endl;
    for (const VAL::goal* g : *conj_goal->getGoals()) {
      PrintGoal(os, g, depth + 1);
    }
    return;
  }

  // Disjunction
  const auto* disj_goal = dynamic_cast<const VAL::disj_goal*>(goal);
  if (disj_goal != nullptr) {
    os << padding << "or:" << std::endl;
    for (const VAL::goal* g : *disj_goal->getGoals()) {
      PrintGoal(os, g, depth + 1);
    }
    return;
  }

  // Negation
  const auto* neg_goal = dynamic_cast<const VAL::neg_goal*>(goal);
  if (neg_goal != nullptr) {
    os << padding << "neg:" << std::endl;
    PrintGoal(os, neg_goal->getGoal(), depth + 1);
    return;
  }

  // Quantification
  const auto* qfied_goal = dynamic_cast<const VAL::qfied_goal*>(goal);
  if (qfied_goal != nullptr) {
    std::string quantifier;
    switch (qfied_goal->getQuantifier()) {
      case VAL::quantifier::E_FORALL:
        quantifier = "forall";
        break;
      case VAL::quantifier::E_EXISTS:
        quantifier = "exists";
        break;
    }
    os << padding << quantifier << *qfied_goal->getVars() << ":" << std::endl;
    PrintGoal(os, qfied_goal->getGoal(), depth + 1);
    return;
  }

  const auto* con_goal = dynamic_cast<const VAL::con_goal*>(goal);
  const auto* constraint_goal = dynamic_cast<const VAL::constraint_goal*>(goal);
  const auto* preference = dynamic_cast<const VAL::preference*>(goal);
  const auto* imply_goal = dynamic_cast<const VAL::imply_goal*>(goal);
  const auto* timed_goal = dynamic_cast<const VAL::timed_goal*>(goal);
  const auto* comparison = dynamic_cast<const VAL::comparison*>(goal);
  os << "con_goal: " << con_goal << std::endl;
  os << "constraint_goal: " << constraint_goal << std::endl;
  os << "preference: " << preference << std::endl;
  os << "disj_goal: " << disj_goal << std::endl;
  os << "imply_goal: " << imply_goal << std::endl;
  os << "timed_goal: " << timed_goal << std::endl;
  os << "comparison: " << comparison << std::endl;

  throw std::runtime_error("PrintGoal(): Goal type not implemented.");
}

void PrintEffects(std::ostream& os, const VAL::effect_lists* effects,
                  size_t depth) {
  std::string padding(depth, '\t');
  for (const VAL::simple_effect* effect : effects->add_effects) {
    os << padding << "(+) " << *effect << std::endl;
  }
  for (const VAL::simple_effect* effect : effects->del_effects) {
    os << padding << "(-) " << *effect << std::endl;
  }
  for (const VAL::forall_effect* effect : effects->forall_effects) {
    os << padding << "forall" << *effect->getVarsList() << ":" << std::endl;
    PrintEffects(os, effect->getEffects(), depth + 1);
  }
  for (const VAL::cond_effect* effect : effects->cond_effects) {
    os << padding << "when:" << std::endl;
    PrintGoal(os, effect->getCondition(), depth + 1);
    os << padding << "then:" << std::endl;
    PrintEffects(os, effect->getEffects(), depth + 1);
  }
}

template <typename T>
void PrintArgs(std::ostream& os, const VAL::typed_symbol_list<T>& args) {
  std::string separator;
  os << "(";
  for (const VAL::parameter_symbol* param : args) {
    os << separator << param->getName() << " [" << param
       << "]: " << param->type->getName();
    if (separator.empty()) separator = ", ";
  }
  os << ")";
}

}  // namespace

namespace VAL {

std::ostream& operator<<(std::ostream& os, const VAL::domain& domain) {
  os << "DOMAIN" << std::endl;
  os << "======" << std::endl;
  os << "Name: " << domain.name << std::endl;

  os << "Requirements: " << VAL::pddl_req_flags_string(domain.req) << std::endl;

  os << "Types: " << std::endl;
  if (domain.types != nullptr) {
    for (const VAL::pddl_type* type : *domain.types) {
      os << "\t" << type->getName() << ": " << type->type->getName() << " ["
         << type << "]" << std::endl;
    }
  }

  os << "Constants: " << std::endl;
  if (domain.constants != nullptr) {
    for (const VAL::const_symbol* c : *domain.constants) {
      os << "\t" << c->getName() << " [" << c << "]"
         << ": " << c->type->getName() << std::endl;
    }
  }

  os << "Predicates:" << std::endl;
  if (domain.predicates != nullptr) {
    for (const VAL::pred_decl* pred : *domain.predicates) {
      os << "\t" << pred->getPred()->getName() << *pred->getArgs() << " ["
         << pred << "]" << std::endl;
    }
  }

  os << "Actions: " << std::endl;
  if (domain.ops != nullptr) {
    for (const VAL::operator_* op : *domain.ops) {
      os << "\t" << op->name->getName() << *op->parameters << std::endl;

      os << "\t\tPreconditions:" << std::endl;
      PrintGoal(os, op->precondition, 3);

      os << "\t\tEffects:" << std::endl;
      PrintEffects(os, op->effects, 3);
    }
  }

  return os;
}

std::ostream& operator<<(std::ostream& os, const VAL::problem& problem) {
  os << "PROBLEM" << std::endl;
  os << "=======" << std::endl;
  os << "Name: " << problem.name << std::endl;

  os << "Domain: " << problem.domain_name << std::endl;

  os << "Requirements: " << VAL::pddl_req_flags_string(problem.req)
     << std::endl;

  os << "Objects:" << std::endl;
  for (const VAL::const_symbol* object : *problem.objects) {
    os << "\t" << object->getName() << " [" << object << "]"
       << ": " << object->type->getName() << std::endl;
  }

  os << "Initial State:" << std::endl;
  PrintEffects(os, problem.initial_state, 1);

  os << "Goal:" << std::endl;
  PrintGoal(os, problem.the_goal, 1);

  return os;
}

std::ostream& operator<<(std::ostream& os, const VAL::simple_effect& effect) {
  os << effect.prop->head->getName() << *effect.prop->args << " ["
     << effect.prop->head << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, const VAL::var_symbol_list& args) {
  PrintArgs(os, args);
  return os;
}

std::ostream& operator<<(std::ostream& os,
                         const VAL::parameter_symbol_list& args) {
  PrintArgs(os, args);
  return os;
}

}  // namespace VAL

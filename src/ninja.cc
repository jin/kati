// Copyright 2015 Google Inc. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build ignore

#include "ninja.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "command.h"
#include "dep.h"
#include "eval.h"
#include "file_cache.h"
#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "func.h"
#include "io.h"
#include "log.h"
#include "stats.h"
#include "string_piece.h"
#include "stringprintf.h"
#include "strutil.h"
#include "thread_pool.h"
#include "timeutil.h"
#include "var.h"
#include "version.h"

static size_t FindCommandLineFlag(StringPiece cmd, StringPiece name) {
  const size_t found = cmd.find(name);
  if (found == string::npos || found == 0)
    return string::npos;
  return found;
}

static StringPiece FindCommandLineFlagWithArg(StringPiece cmd,
                                              StringPiece name) {
  size_t index = FindCommandLineFlag(cmd, name);
  if (index == string::npos)
    return StringPiece();

  StringPiece val = TrimLeftSpace(cmd.substr(index + name.size()));
  index = val.find(name);
  while (index != string::npos) {
    val = TrimLeftSpace(val.substr(index + name.size()));
    index = val.find(name);
  }

  index = val.find_first_of(" \t");
  return val.substr(0, index);
}

static bool StripPrefix(StringPiece p, StringPiece* s) {
  if (!HasPrefix(*s, p))
    return false;
  *s = s->substr(p.size());
  return true;
}

size_t GetGomaccPosForAndroidCompileCommand(StringPiece cmdline) {
  size_t index = cmdline.find(' ');
  if (index == string::npos)
    return string::npos;
  StringPiece cmd = cmdline.substr(0, index);
  if (HasSuffix(cmd, "ccache")) {
    index++;
    size_t pos = GetGomaccPosForAndroidCompileCommand(cmdline.substr(index));
    return pos == string::npos ? string::npos : pos + index;
  }
  if (!StripPrefix("prebuilts/", &cmd))
    return string::npos;
  if (!StripPrefix("gcc/", &cmd) && !StripPrefix("clang/", &cmd))
    return string::npos;
  if (!HasSuffix(cmd, "gcc") && !HasSuffix(cmd, "g++") &&
      !HasSuffix(cmd, "clang") && !HasSuffix(cmd, "clang++")) {
    return string::npos;
  }

  StringPiece rest = cmdline.substr(index);
  return rest.find(" -c ") != string::npos ? 0 : string::npos;
}

static bool GetDepfileFromCommandImpl(StringPiece cmd, string* out) {
  if ((FindCommandLineFlag(cmd, " -MD") == string::npos &&
       FindCommandLineFlag(cmd, " -MMD") == string::npos) ||
      FindCommandLineFlag(cmd, " -c") == string::npos) {
    return false;
  }

  StringPiece mf = FindCommandLineFlagWithArg(cmd, " -MF");
  if (!mf.empty()) {
    mf.AppendToString(out);
    return true;
  }

  StringPiece o = FindCommandLineFlagWithArg(cmd, " -o");
  if (o.empty()) {
    ERROR("Cannot find the depfile in %s", cmd.as_string().c_str());
    return false;
  }

  StripExt(o).AppendToString(out);
  *out += ".d";
  return true;
}

bool GetDepfileFromCommand(string* cmd, string* out) {
  CHECK(!cmd->empty());
  if (!GetDepfileFromCommandImpl(*cmd, out))
    return false;

  // A hack for Android - llvm-rs-cc seems not to emit a dep file.
  if (cmd->find("bin/llvm-rs-cc ") != string::npos) {
    return false;
  }

  // TODO: A hack for Makefiles generated by automake.

  // A hack for Android to get .P files instead of .d.
  string p;
  StripExt(*out).AppendToString(&p);
  p += ".P";
  if (cmd->find(p) != string::npos) {
    const string rm_f = "; rm -f " + *out;
    const size_t found = cmd->find(rm_f);
    if (found == string::npos) {
      ERROR("Cannot find removal of .d file: %s", cmd->c_str());
    }
    cmd->erase(found, rm_f.size());
    return true;
  }

  // A hack for Android. For .s files, GCC does not use C
  // preprocessor, so it ignores -MF flag.
  string as = "/";
  StripExt(Basename(*out)).AppendToString(&as);
  as += ".s";
  if (cmd->find(as) != string::npos) {
    return false;
  }

  *cmd += "&& cp ";
  *cmd += *out;
  *cmd += ' ';
  *cmd += *out;
  *cmd += ".tmp ";
  *out += ".tmp";
  return true;
}

struct NinjaNode {
  const DepNode* node;
  vector<Command*> commands;
  int rule_id;
};

class NinjaGenerator {
 public:
  NinjaGenerator(Evaluator* ev, double start_time)
      : ce_(ev),
        ev_(ev),
        fp_(NULL),
        rule_id_(0),
        start_time_(start_time),
        default_target_(NULL) {
    ev_->set_avoid_io(true);
    shell_ = EscapeNinja(ev->GetShell());
    shell_flags_ = EscapeNinja(ev->GetShellFlag());
    const string use_goma_str = ev->EvalVar(Intern("USE_GOMA"));
    use_goma_ = !(use_goma_str.empty() || use_goma_str == "false");
    if (g_flags.goma_dir)
      gomacc_ = StringPrintf("%s/gomacc ", g_flags.goma_dir);

    GetExecutablePath(&kati_binary_);
  }

  ~NinjaGenerator() {
    ev_->set_avoid_io(false);
    for (NinjaNode* nn : nodes_)
      delete nn;
  }

  void Generate(const vector<NamedDepNode>& nodes, const string& orig_args) {
    unlink(GetNinjaStampFilename().c_str());
    PopulateNinjaNodes(nodes);
    GenerateNinja();
    GenerateShell();
    GenerateStamp(orig_args);
  }

  static string GetStampTempFilename() {
    return GetFilename(".kati_stamp%s.tmp");
  }

  static string GetFilename(const char* fmt) {
    string r = g_flags.ninja_dir ? g_flags.ninja_dir : ".";
    r += '/';
    r += StringPrintf(fmt, g_flags.ninja_suffix ? g_flags.ninja_suffix : "");
    return r;
  }

 private:
  void PopulateNinjaNodes(const vector<NamedDepNode>& nodes) {
    ScopedTimeReporter tr("ninja gen (eval)");
    for (auto const& node : nodes) {
      PopulateNinjaNode(node.second);
    }
  }

  void PopulateNinjaNode(DepNode* node) {
    if (done_.exists(node->output)) {
      return;
    }
    done_.insert(node->output);
    ScopedFrame frame(ce_.evaluator()->Enter(FrameType::NINJA,
                                             node->output.str(), node->loc));

    // A hack to exclude out phony target in Android. If this exists,
    // "ninja -t clean" tries to remove this directory and fails.
    if (g_flags.detect_android_echo && node->output.str() == "out")
      return;

    // This node is a leaf node
    if (!node->has_rule && !node->is_phony) {
      return;
    }

    NinjaNode* nn = new NinjaNode;
    nn->node = node;
    ce_.Eval(node, &nn->commands);
    nn->rule_id = nn->commands.empty() ? -1 : rule_id_++;
    nodes_.push_back(nn);

    for (auto const& d : node->deps) {
      PopulateNinjaNode(d.second);
    }
    for (auto const& d : node->order_onlys) {
      PopulateNinjaNode(d.second);
    }
    for (auto const& d : node->validations) {
      PopulateNinjaNode(d.second);
    }
  }

  StringPiece TranslateCommand(const char* in, string* cmd_buf) {
    const size_t orig_size = cmd_buf->size();
    bool prev_backslash = false;
    // Set space as an initial value so the leading comment will be
    // stripped out.
    char prev_char = ' ';
    char quote = 0;
    for (; *in; in++) {
      switch (*in) {
        case '#':
          if (quote == 0 && isspace(prev_char)) {
            while (in[1] && *in != '\n')
              in++;
          } else {
            *cmd_buf += *in;
          }
          break;

        case '\'':
        case '"':
        case '`':
          if (quote) {
            if (quote == *in)
              quote = 0;
          } else if (!prev_backslash) {
            quote = *in;
          }
          *cmd_buf += *in;
          break;

        case '$':
          *cmd_buf += "$$";
          break;

        case '\n':
          if (prev_backslash) {
            cmd_buf->resize(cmd_buf->size() - 1);
          } else {
            *cmd_buf += ' ';
          }
          break;

        case '\\':
          *cmd_buf += '\\';
          break;

        default:
          *cmd_buf += *in;
      }

      if (*in == '\\') {
        prev_backslash = !prev_backslash;
      } else {
        prev_backslash = false;
      }

      prev_char = *in;
    }

    if (prev_backslash) {
      cmd_buf->resize(cmd_buf->size() - 1);
    }

    while (true) {
      char c = (*cmd_buf)[cmd_buf->size() - 1];
      if (!isspace(c) && c != ';')
        break;
      cmd_buf->resize(cmd_buf->size() - 1);
    }

    return StringPiece(cmd_buf->data() + orig_size,
                       cmd_buf->size() - orig_size);
  }

  bool IsOutputMkdir(const char* name, StringPiece cmd) {
    if (!HasPrefix(cmd, "mkdir -p ")) {
      return false;
    }
    cmd = cmd.substr(9, cmd.size());
    if (cmd.get(cmd.size() - 1) == '/') {
      cmd = cmd.substr(0, cmd.size() - 1);
    }

    StringPiece dir = Dirname(name);
    if (cmd == dir) {
      return true;
    }
    return false;
  }

  bool GetDescriptionFromCommand(StringPiece cmd, string* out) {
    if (!HasPrefix(cmd, "echo ")) {
      return false;
    }
    cmd = cmd.substr(5, cmd.size());

    bool prev_backslash = false;
    char quote = 0;
    string out_buf;

    // Strip outer quotes, and fail if it is not a single echo command
    for (StringPiece::iterator in = cmd.begin(); in != cmd.end(); in++) {
      if (prev_backslash) {
        prev_backslash = false;
        out_buf += *in;
      } else if (*in == '\\') {
        prev_backslash = true;
        out_buf += *in;
      } else if (quote) {
        if (*in == quote) {
          quote = 0;
        } else {
          out_buf += *in;
        }
      } else {
        switch (*in) {
          case '\'':
          case '"':
          case '`':
            quote = *in;
            break;

          case '<':
          case '>':
          case '&':
          case '|':
          case ';':
            return false;

          default:
            out_buf += *in;
        }
      }
    }

    *out = out_buf;
    return true;
  }

  bool GenShellScript(const char* name,
                      const vector<Command*>& commands,
                      string* cmd_buf,
                      string* description) {
    bool got_descritpion = false;
    bool use_gomacc = false;
    auto command_count = commands.size();
    for (const Command* c : commands) {
      size_t cmd_begin = cmd_buf->size();

      if (!cmd_buf->empty()) {
        *cmd_buf += " && ";
      }

      const char* in = c->cmd.c_str();
      while (isspace(*in))
        in++;

      bool needs_subshell = (command_count > 1 || c->ignore_error);

      if (needs_subshell)
        *cmd_buf += '(';

      size_t cmd_start = cmd_buf->size();
      StringPiece translated = TranslateCommand(in, cmd_buf);
      if (g_flags.detect_android_echo && !got_descritpion && !c->echo &&
          GetDescriptionFromCommand(translated, description)) {
        got_descritpion = true;
        translated.clear();
      } else if (IsOutputMkdir(name, translated) && !c->echo &&
                 cmd_begin == 0) {
        translated.clear();
      }
      if (translated.empty()) {
        cmd_buf->resize(cmd_begin);
        command_count -= 1;
        continue;
      } else if (g_flags.goma_dir) {
        size_t pos = GetGomaccPosForAndroidCompileCommand(translated);
        if (pos != string::npos) {
          cmd_buf->insert(cmd_start + pos, gomacc_);
          use_gomacc = true;
        }
      } else if (translated.find("/gomacc") != string::npos) {
        use_gomacc = true;
      }

      if (c->ignore_error) {
        *cmd_buf += " ; true";
      }

      if (needs_subshell)
        *cmd_buf += " )";
    }
    return (use_goma_ || g_flags.remote_num_jobs || g_flags.goma_dir) &&
           !use_gomacc;
  }

  bool GetDepfile(const DepNode* node, string* cmd_buf, string* depfile) {
    if (node->depfile_var) {
      node->depfile_var->Eval(ev_, depfile);
      return true;
    }
    if (!g_flags.detect_depfiles)
      return false;

    *cmd_buf += ' ';
    bool result = GetDepfileFromCommand(cmd_buf, depfile);
    cmd_buf->resize(cmd_buf->size() - 1);
    return result;
  }

  void EmitDepfile(NinjaNode* nn, string* cmd_buf, ostringstream* o) {
    const DepNode* node = nn->node;
    string depfile;
    if (!GetDepfile(node, cmd_buf, &depfile))
      return;
    *o << " depfile = " << depfile << "\n";
    *o << " deps = gcc\n";
  }

  void EmitNode(NinjaNode* nn, ostringstream* o) {
    const DepNode* node = nn->node;
    const vector<Command*>& commands = nn->commands;

    string rule_name = "phony";
    bool use_local_pool = false;
    if (IsSpecialTarget(node->output)) {
      return;
    }
    if (g_flags.enable_debug) {
      *o << "# " << (node->loc.filename ? node->loc.filename : "(null)") << ':'
         << node->loc.lineno << "\n";
    }
    if (!commands.empty()) {
      rule_name = StringPrintf("rule%d", nn->rule_id);
      *o << "rule " << rule_name << "\n";

      string description = "build $out";
      string cmd_buf;
      use_local_pool |= GenShellScript(node->output.c_str(), commands, &cmd_buf,
                                       &description);
      *o << " description = " << description << "\n";
      EmitDepfile(nn, &cmd_buf, o);

      // It seems Linux is OK with ~130kB and Mac's limit is ~250kB.
      // TODO: Find this number automatically.
      if (cmd_buf.size() > 100 * 1000) {
        *o << " rspfile = $out.rsp\n";
        *o << " rspfile_content = " << cmd_buf << "\n";
        *o << " command = " << shell_ << " $out.rsp\n";
      } else {
        EscapeShell(&cmd_buf);
        *o << " command = " << shell_ << ' ' << shell_flags_ << " \"" << cmd_buf
           << "\"\n";
      }
      if (node->is_restat) {
        *o << " restat = 1\n";
      }
    }

    EmitBuild(nn, rule_name, use_local_pool, o);
  }

  string EscapeNinja(const string& s) const {
    if (s.find_first_of("$: ") == string::npos)
      return s;
    string r;
    for (char c : s) {
      switch (c) {
        case '$':
        case ':':
        case ' ':
          r += '$';
#if defined(__has_cpp_attribute) && __has_cpp_attribute(clang::fallthrough)
          [[clang::fallthrough]];
#endif
        default:
          r += c;
      }
    }
    return r;
  }

  string EscapeBuildTarget(Symbol s) const { return EscapeNinja(s.str()); }

  void EmitBuild(NinjaNode* nn,
                 const string& rule_name,
                 bool use_local_pool,
                 ostringstream* o) {
    const DepNode* node = nn->node;
    string target = EscapeBuildTarget(node->output);
    *o << "build " << target;
    if (!node->implicit_outputs.empty()) {
      *o << " |";
      for (Symbol output : node->implicit_outputs) {
        *o << " " << EscapeBuildTarget(output);
      }
    }
    *o << ": " << rule_name;
    vector<Symbol> order_onlys;
    if (node->is_phony && !g_flags.use_ninja_phony_output) {
      *o << " _kati_always_build_";
    }
    for (auto const& d : node->deps) {
      *o << " " << EscapeBuildTarget(d.first).c_str();
    }
    if (!node->order_onlys.empty()) {
      *o << " ||";
      for (auto const& d : node->order_onlys) {
        *o << " " << EscapeBuildTarget(d.first).c_str();
      }
    }
    if (!node->validations.empty()) {
      *o << " |@";
      for (auto const& d : node->validations) {
        *o << " " << EscapeBuildTarget(d.first).c_str();
      }
    }

    *o << "\n";

    if (!node->symlink_outputs.empty()) {
      *o << " symlink_outputs =";
      for (auto const& s : node->symlink_outputs) {
        *o << " " << EscapeBuildTarget(s);
      }
      *o << "\n";
    }

    string pool;
    if (node->ninja_pool_var) {
      node->ninja_pool_var->Eval(ev_, &pool);
    }

    if (pool != "") {
      if (pool != "none") {
        *o << " pool = " << pool << "\n";
      }
    } else if (g_flags.default_pool && rule_name != "phony") {
      *o << " pool = " << g_flags.default_pool << "\n";
    } else if (use_local_pool) {
      *o << " pool = local_pool\n";
    }
    if (node->is_phony && g_flags.use_ninja_phony_output) {
      *o << " phony_output = true\n";
    }
    if (node->is_default_target) {
      unique_lock<mutex> lock(mu_);
      default_target_ = node;
    }
  }

  static string GetEnvScriptFilename() { return GetFilename("env%s.sh"); }

  void GenerateNinja() {
    ScopedTimeReporter tr("ninja gen (emit)");
    fp_ = fopen(GetNinjaFilename().c_str(), "wb");
    if (fp_ == NULL)
      PERROR("fopen(build.ninja) failed");

    fprintf(fp_, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp_, "\n");

    if (!used_envs_.empty()) {
      fprintf(fp_, "# Environment variables used:\n");
      for (const auto& p : used_envs_) {
        fprintf(fp_, "# %s=%s\n", p.first.c_str(), p.second.c_str());
      }
      fprintf(fp_, "\n");
    }

    if (!g_flags.no_ninja_prelude) {
      if (g_flags.ninja_dir) {
        fprintf(fp_, "builddir = %s\n\n", g_flags.ninja_dir);
      }

      fprintf(fp_, "pool local_pool\n");
      fprintf(fp_, " depth = %d\n\n", g_flags.num_jobs);

      if (!g_flags.use_ninja_phony_output) {
        fprintf(fp_, "build _kati_always_build_: phony\n\n");
      }
    }

    unique_ptr<ThreadPool> tp(NewThreadPool(g_flags.num_jobs));
    CHECK(g_flags.num_jobs);
    int num_nodes_per_task = nodes_.size() / (g_flags.num_jobs * 10) + 1;
    int num_tasks = nodes_.size() / num_nodes_per_task + 1;
    vector<ostringstream> bufs(num_tasks);
    for (int i = 0; i < num_tasks; i++) {
      tp->Submit([this, i, num_nodes_per_task, &bufs]() {
        int l =
            min(num_nodes_per_task * (i + 1), static_cast<int>(nodes_.size()));
        for (int j = num_nodes_per_task * i; j < l; j++) {
          EmitNode(nodes_[j], &bufs[i]);
        }
      });
    }
    tp->Wait();

    if (!g_flags.generate_empty_ninja) {
      for (const ostringstream& buf : bufs) {
        fprintf(fp_, "%s", buf.str().c_str());
      }
    }

    SymbolSet used_env_vars(Vars::used_env_vars());
    // PATH changes $(shell).
    used_env_vars.insert(Intern("PATH"));
    for (Symbol e : used_env_vars) {
      StringPiece val(getenv(e.c_str()));
      used_envs_.emplace(e.str(), val.as_string());
    }

    string default_targets;
    if (g_flags.targets.empty() || g_flags.gen_all_targets) {
      CHECK(default_target_);
      default_targets = EscapeBuildTarget(default_target_->output);
    } else {
      for (Symbol s : g_flags.targets) {
        if (!default_targets.empty())
          default_targets += ' ';
        default_targets += EscapeBuildTarget(s);
      }
    }
    if (!g_flags.generate_empty_ninja) {
      fprintf(fp_, "\n");
      fprintf(fp_, "default %s\n", default_targets.c_str());
    }

    fclose(fp_);
  }

  void GenerateShell() {
    FILE* fp = fopen(GetEnvScriptFilename().c_str(), "wb");
    if (fp == NULL)
      PERROR("fopen(env.sh) failed");

    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp, "\n");

    for (const auto& p : ev_->exports()) {
      if (p.second) {
        const string val = ev_->EvalVar(p.first);
        fprintf(fp, "export '%s'='%s'\n", p.first.c_str(), val.c_str());
      } else {
        fprintf(fp, "unset '%s'\n", p.first.c_str());
      }
    }

    fclose(fp);

    fp = fopen(GetNinjaShellScriptFilename().c_str(), "wb");
    if (fp == NULL)
      PERROR("fopen(ninja.sh) failed");

    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp, "\n");

    fprintf(fp, ". %s\n", GetEnvScriptFilename().c_str());

    fprintf(fp, "exec ninja -f %s ", GetNinjaFilename().c_str());
    if (g_flags.remote_num_jobs > 0) {
      fprintf(fp, "-j%d ", g_flags.remote_num_jobs);
    } else if (g_flags.goma_dir) {
      fprintf(fp, "-j500 ");
    }
    fprintf(fp, "\"$@\"\n");

    fclose(fp);

    if (chmod(GetNinjaShellScriptFilename().c_str(), 0755) != 0)
      PERROR("chmod ninja.sh failed");
  }

  void GenerateStamp(const string& orig_args) {
    FILE* fp = fopen(GetStampTempFilename().c_str(), "wb");
    CHECK(fp);

    size_t r = fwrite(&start_time_, sizeof(start_time_), 1, fp);
    CHECK(r == 1);

    unordered_set<string> makefiles;
    MakefileCacheManager::Get()->GetAllFilenames(&makefiles);
    DumpInt(fp, makefiles.size() + 1);
    DumpString(fp, kati_binary_);
    for (const string& makefile : makefiles) {
      DumpString(fp, makefile);
    }

    DumpInt(fp, Evaluator::used_undefined_vars().size());
    for (Symbol v : Evaluator::used_undefined_vars()) {
      DumpString(fp, v.str());
    }
    DumpInt(fp, used_envs_.size());
    for (const auto& p : used_envs_) {
      DumpString(fp, p.first);
      DumpString(fp, p.second);
    }

    const unordered_map<string, vector<string>*>& globs = GetAllGlobCache();
    DumpInt(fp, globs.size());
    for (const auto& p : globs) {
      DumpString(fp, p.first);
      const vector<string>& files = *p.second;
#if 0
      unordered_set<string> dirs;
      GetReadDirs(p.first, files, &dirs);
      DumpInt(fp, dirs.size());
      for (const string& dir : dirs) {
        DumpString(fp, dir);
      }
#endif
      DumpInt(fp, files.size());
      for (const string& file : files) {
        DumpString(fp, file);
      }
    }

    const vector<CommandResult*>& crs = GetShellCommandResults();
    DumpInt(fp, crs.size());
    for (CommandResult* cr : crs) {
      DumpInt(fp, static_cast<int>(cr->op));
      DumpString(fp, cr->shell);
      DumpString(fp, cr->shellflag);
      DumpString(fp, cr->cmd);
      DumpString(fp, cr->result);
      DumpString(fp, cr->loc.filename);
      DumpInt(fp, cr->loc.lineno);

      if (cr->op == CommandOp::FIND) {
        vector<string> missing_dirs;
        for (StringPiece fd : cr->find->finddirs) {
          const string& d = ConcatDir(cr->find->chdir, fd);
          if (!Exists(d))
            missing_dirs.push_back(d);
        }
        DumpInt(fp, missing_dirs.size());
        for (const string& d : missing_dirs) {
          DumpString(fp, d);
        }

        DumpInt(fp, cr->find->found_files->size());
        for (StringPiece s : *cr->find->found_files) {
          DumpString(fp, ConcatDir(cr->find->chdir, s));
        }

        DumpInt(fp, cr->find->read_dirs->size());
        for (StringPiece s : *cr->find->read_dirs) {
          DumpString(fp, ConcatDir(cr->find->chdir, s));
        }
      }
    }

    DumpString(fp, orig_args);

    fclose(fp);

    rename(GetStampTempFilename().c_str(), GetNinjaStampFilename().c_str());
  }

  CommandEvaluator ce_;
  Evaluator* ev_;
  FILE* fp_;
  SymbolSet done_;
  int rule_id_;
  bool use_goma_;
  string gomacc_;
  string shell_;
  string shell_flags_;
  map<string, string> used_envs_;
  string kati_binary_;
  const double start_time_;
  vector<NinjaNode*> nodes_;

  mutex mu_;
  const DepNode* default_target_;
};

string GetNinjaFilename() {
  return NinjaGenerator::GetFilename("build%s.ninja");
}

string GetNinjaShellScriptFilename() {
  return NinjaGenerator::GetFilename("ninja%s.sh");
}

string GetNinjaStampFilename() {
  return NinjaGenerator::GetFilename(".kati_stamp%s");
}

void GenerateNinja(const vector<NamedDepNode>& nodes,
                   Evaluator* ev,
                   const string& orig_args,
                   double start_time) {
  NinjaGenerator ng(ev, start_time);
  ng.Generate(nodes, orig_args);
}

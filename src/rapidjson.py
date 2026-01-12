# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from extension_helper import ExtensionHelper

hook = ExtensionHelper('rapidjson', globals())
hook.FetchFromGit('https://github.com/Tencent/rapidjson.git', 'master')
hook.SkipConfigure()
hook.InstallWhenIncluded(r'rapidjson/.*\.h')
hook.AddCompileArgs('-I', hook.src_dir / 'include')
hook.AddCompileArgs('-DRAPIDJSON_HAS_STDSTRING')

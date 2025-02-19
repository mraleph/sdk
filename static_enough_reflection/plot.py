# Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

df = pd.read_csv('/tmp/results3.csv', names=['numclasses','what','mode','variant','result'])

def PlotData(df, what, mode, title, measure):
    print(f'# {what} {mode} -------')
    df = df[df['what'] == what]
    df = df[df['mode'] == mode]

    plt.figure(figsize=(10, 6))
    sns.lineplot(x='numclasses', y='result', hue='variant', data=df, errorbar='sd')
    plt.xlabel('# of Classes')
    plt.ylabel(measure)
    plt.title(title)
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.legend(title='Variant')
    plt.tight_layout()
    plt.savefig(f'/tmp/results.{what}.{mode}.png')

    print(df[df['numclasses'] == 1024][['variant', 'result']].groupby('variant').describe())
    print('')

PlotData(df, 'gen_kernel', 'jit', title='gen_kernel(JIT) time', measure='ms')
PlotData(df, 'gen_kernel', 'aot', title='gen_kernel(AOT) time', measure='ms')
PlotData(df, 'gen_snapshot', 'aot', title='gen_snapshot time', measure='ms')
PlotData(df, 'size', 'aot', title='snapshot size', measure='bytes')

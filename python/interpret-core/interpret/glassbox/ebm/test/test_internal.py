# Copyright (c) 2019 Microsoft Corporation
# Distributed under the MIT software license

from ..native import Native, Booster

import numpy as np
import ctypes as ct
from contextlib import closing

def test_booster_internals():
    with Booster(
        model_type="classification",
        n_classes=2,
        features_categorical=np.array([0], dtype=ct.c_int64, order="C"), 
        features_bin_count=np.array([2], dtype=ct.c_int64, order="C"),
        feature_groups=[[0]],
        X_train=np.array([[0]], dtype=ct.c_int64, order="C"),
        y_train=np.array([0], dtype=ct.c_int64, order="C"),
        w_train=np.array([1], dtype=np.float64, order="C"),
        scores_train=None,
        X_val=np.array([[0]], dtype=ct.c_int64, order="C"),
        y_val=np.array([0], dtype=ct.c_int64, order="C"),
        w_val=np.array([1], dtype=np.float64, order="C"),
        scores_val=None,
        n_inner_bags=0,
        random_state=42,
        optional_temp_params=None,
    ) as booster:
        gain = booster.generate_model_update(
            feature_group_index=0,
            generate_update_options=Native.GenerateUpdateOptions_Default,
            learning_rate=0.01,
            min_samples_leaf=2,
            max_leaves=np.array([2], dtype=ct.c_int64, order="C"),
        )
        assert gain == 0

        splits = booster.get_model_update_splits()
        assert len(splits) == 1
        assert len(splits[0]) == 0

        model_update = booster.get_model_update_expanded()
        assert len(model_update.shape) == 1
        assert model_update.shape[0] == 2
        assert model_update[0] < 0

        booster.set_model_update_expanded(0, model_update)

        metric = booster.apply_model_update()
        assert 0 < metric

        model = booster.get_best_model()
        assert len(model) == 1
        assert len(model[0].shape) == 1
        assert model[0].shape[0] == 2
        assert model[0][0] < 0


def test_one_class():
    with Booster(
        model_type="classification",
        n_classes=1,
        features_categorical=np.array([0], dtype=ct.c_int64, order="C"), 
        features_bin_count=np.array([2], dtype=ct.c_int64, order="C"),
        feature_groups=[[0]],
        X_train=np.array([[0, 1, 0]], dtype=ct.c_int64, order="C"),
        y_train=np.array([0, 0, 0], dtype=ct.c_int64, order="C"),
        w_train=np.array([1, 1, 1], dtype=np.float64, order="C"),
        scores_train=None,
        X_val=np.array([[1, 0, 1]], dtype=ct.c_int64, order="C"),
        y_val=np.array([0, 0, 0], dtype=ct.c_int64, order="C"),
        w_val=np.array([1, 1, 1], dtype=np.float64, order="C"),
        scores_val=None,
        n_inner_bags=0,
        random_state=42,
        optional_temp_params=None,
    ) as booster:
        gain = booster.generate_model_update(
            feature_group_index=0,
            generate_update_options=Native.GenerateUpdateOptions_Default,
            learning_rate=0.01,
            min_samples_leaf=2,
            max_leaves=np.array([2], dtype=ct.c_int64, order="C"),
        )
        assert gain == 0

        splits = booster.get_model_update_splits()
        assert len(splits) == 1
        assert len(splits[0]) == 0

        model_update = booster.get_model_update_expanded()
        assert model_update is None

        booster.set_model_update_expanded(0, model_update)

        metric = booster.apply_model_update()
        assert metric == 0

        model = booster.get_best_model()
        assert len(model) == 1
        assert model[0] is None

def test_suggest_graph_bound():
    native = Native.get_native_singleton()
    cuts=[25, 50, 75]
    (low_graph_bound, high_graph_bound) = native.suggest_graph_bounds(cuts, 24, 76)
    assert low_graph_bound < 25
    assert 75 < high_graph_bound

def test_suggest_graph_bound_no_min_max():
    native = Native.get_native_singleton()
    cuts=[25, 50, 75]
    (low_graph_bound, high_graph_bound) = native.suggest_graph_bounds(cuts)
    assert low_graph_bound < 25
    assert 75 < high_graph_bound

def test_suggest_graph_bound_no_cuts():
    native = Native.get_native_singleton()
    cuts=[]
    (low_graph_bound, high_graph_bound) = native.suggest_graph_bounds(cuts, 24, 76)
    assert low_graph_bound <= 24
    assert 76 <= high_graph_bound




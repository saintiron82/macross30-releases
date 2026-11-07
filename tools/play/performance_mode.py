"""Explicit geometry policy; diagnostic comparison never leaks into normal play."""


def apply_performance_mode(environment, mode='trusted'):
    if mode not in ('trusted','verify'):raise ValueError('Unknown performance mode')
    environment.pop('EDGE_GEOM6_EVIDENCE',None)  # opt-in diagnostic overrides are applied afterwards
    environment.update(EDGE_NATIVE='1',EDGE_SKIN='1',EDGE_VERIFY='1' if mode=='verify' else '0')


def performance_settings(environment):
    prefixes=('EDGE_','RIBBON_','MACROSS_','SPURS_','SPU_','LD_','FIFO_','RSX_','GCM_','D3D12_','AUDIO_')
    return {key:value for key,value in sorted(environment.items()) if key.startswith(prefixes)}


def effective_performance_mode(environment):
    value=environment.get('EDGE_VERIFY','')
    return 'verify' if value and value[0]!='0' else 'trusted'

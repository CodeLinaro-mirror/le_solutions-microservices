# StopConfiguration

Not supported with latest reasoning models `o3` and `o4-mini`.  Up to 4 sequences where the API will stop generating further tokens. The returned text will not contain the stop sequence. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.stop_configuration import StopConfiguration

# TODO update the JSON string below
json = "{}"
# create an instance of StopConfiguration from a JSON string
stop_configuration_instance = StopConfiguration.from_json(json)
# print the JSON string representation of the object
print(StopConfiguration.to_json())

# convert the object into a dict
stop_configuration_dict = stop_configuration_instance.to_dict()
# create an instance of StopConfiguration from a dict
stop_configuration_from_dict = StopConfiguration.from_dict(stop_configuration_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)



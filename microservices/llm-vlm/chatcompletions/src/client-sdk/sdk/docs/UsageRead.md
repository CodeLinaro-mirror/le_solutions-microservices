# UsageRead


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**date_bin** | **datetime** |  | 
**total_generation_time** | **float** |  | 
**total_input_tokens** | **int** |  | 
**total_output_tokens** | **int** |  | 
**total_tokens** | **int** |  | 
**userid** | **str** |  | 

## Example

```python
from openapi_client.models.usage_read import UsageRead

# TODO update the JSON string below
json = "{}"
# create an instance of UsageRead from a JSON string
usage_read_instance = UsageRead.from_json(json)
# print the JSON string representation of the object
print(UsageRead.to_json())

# convert the object into a dict
usage_read_dict = usage_read_instance.to_dict()
# create an instance of UsageRead from a dict
usage_read_from_dict = UsageRead.from_dict(usage_read_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)



# CreateRunRequestTruncationStrategy


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The truncation strategy to use for the thread. The default is &#x60;auto&#x60;. If set to &#x60;last_messages&#x60;, the thread will be truncated to the n most recent messages in the thread. When set to &#x60;auto&#x60;, messages in the middle of the thread will be dropped to fit the context length of the model, &#x60;max_prompt_tokens&#x60;. | 
**last_messages** | **int** | The number of most recent messages from the thread when constructing the context for the run. | [optional] 

## Example

```python
from openapi_client.models.create_run_request_truncation_strategy import CreateRunRequestTruncationStrategy

# TODO update the JSON string below
json = "{}"
# create an instance of CreateRunRequestTruncationStrategy from a JSON string
create_run_request_truncation_strategy_instance = CreateRunRequestTruncationStrategy.from_json(json)
# print the JSON string representation of the object
print(CreateRunRequestTruncationStrategy.to_json())

# convert the object into a dict
create_run_request_truncation_strategy_dict = create_run_request_truncation_strategy_instance.to_dict()
# create an instance of CreateRunRequestTruncationStrategy from a dict
create_run_request_truncation_strategy_from_dict = CreateRunRequestTruncationStrategy.from_dict(create_run_request_truncation_strategy_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


